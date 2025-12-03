/*
 * Simple virtio-blk DMA benchmark using qtest/libqos
 *
 * 思路：
 *  - 使用 qgraph 上的 "virtio-blk" 节点获取 QVirtioBlk*（里面有 QVirtioDevice *vdev）
 *  - 创建 queue0，设置 DRIVER_OK
 *  - load 阶段：顺序写若干 4K 块（OUT 请求）
 *  - run 阶段：随机读写若干 4K 块（IN/OUT 混合），记录每次操作耗时
 *  - 运行过程中，后端的 virtio.c 会触发 DMA 映射，从而写入 /tmp/virtio_dma.log
 *
 * 注意：
 *  - 这里只是一个骨架，参数/统计逻辑你可以按需要扩展。
 */

 #include "qemu/osdep.h"
 #include "libqtest-single.h"
 #include "qemu/module.h"
 #include "qemu/bswap.h"
 #include "qemu/timer.h"
 
#include "libqos/qgraph.h"
#include "libqos/virtio-blk.h"
#include "libqos/virtio.h"
 
 #include "standard-headers/linux/virtio_blk.h"
 #include "standard-headers/linux/virtio_ring.h"
 
 #define TEST_IMAGE_SIZE         (64 * 1024 * 1024ULL)  /* 64 MiB */
 #define BLOCK_SIZE              4096                  /* 每次 IO 4K */
 #define LOAD_BLOCKS             1024                  /* load 阶段顺序写多少块 */
 #define RUN_OPS                 10000                 /* run 阶段总操作数 */
 #define RUN_WRITE_RATIO         50                    /* 写操作比例(百分比) */
 #define QVIRTIO_BLK_TIMEOUT_US  (30 * 1000 * 1000ULL)
 
 #if HOST_BIG_ENDIAN
 static const bool host_is_big_endian = true;
 #else
 static const bool host_is_big_endian; /* false */
 #endif
 
 typedef struct QVirtioBlkReq {
     uint32_t type;
     uint32_t ioprio;
     uint64_t sector;
     char *data;
     uint8_t status;
 } QVirtioBlkReq;
 
 /* 简单复制自 virtio-blk-test.c 的格式修正逻辑 */
 static inline void virtio_blk_fix_request(QVirtioDevice *d, QVirtioBlkReq *req)
 {
     if (qvirtio_is_big_endian(d) != host_is_big_endian) {
         req->type = bswap32(req->type);
         req->ioprio = bswap32(req->ioprio);
         req->sector = bswap64(req->sector);
     }
 }
 
 /* 根据 req + data_size 构造 guest 内存里的请求布局，返回 GPA */
 static uint64_t virtio_blk_build_request(QGuestAllocator *alloc,
                                          QVirtioDevice *d,
                                          QVirtioBlkReq *req,
                                          uint64_t data_size)
 {
     uint64_t addr;
     uint8_t status = 0xff;
 
     g_assert_cmpuint(data_size % 512, ==, 0);
 
     addr = guest_alloc(alloc, sizeof(*req) + data_size + sizeof(status));
 
     virtio_blk_fix_request(d, req);
     /* 头（16 字节：type/ioprio/sector/reserved） */
     memwrite(addr, req, 16);
     /* 数据区 */
     if (data_size && req->data) {
         memwrite(addr + 16, req->data, data_size);
     }
     /* status 字节 */
     memwrite(addr + 16 + data_size, &status, sizeof(status));
 
     return addr;
 }
 
 /* 建立 queue0、协商特性等，返回 vq */
 static QVirtQueue *virtio_blk_setup_queues(QVirtioDevice *dev,
                                            QGuestAllocator *alloc)
 {
     uint64_t features;
     uint64_t capacity;
     QVirtQueue *vq;
 
     /* 简单的特性协商，去掉一些复杂特性 */
     features = qvirtio_get_features(dev);
     features &= ~(QVIRTIO_F_BAD_FEATURE |
                   (1u << VIRTIO_RING_F_INDIRECT_DESC) |
                   (1u << VIRTIO_RING_F_EVENT_IDX) |
                   (1u << VIRTIO_BLK_F_SCSI));
     qvirtio_set_features(dev, features);
 
     /* 读一下 capacity 做 sanity check */
     capacity = qvirtio_config_readq(dev, 0);
     g_assert_cmpint(capacity, >=, TEST_IMAGE_SIZE / 512);
 
     vq = qvirtqueue_setup(dev, alloc, 0);
     qvirtio_set_driver_ok(dev);
 
     return vq;
 }
 
/* load 阶段：顺序写 load_ops 个 4K 块（受队列容量限制） */
 static void virtio_blk_load_phase(QVirtioDevice *dev,
                                   QGuestAllocator *alloc,
                                  QVirtQueue *vq,
                                  uint64_t load_ops)
 {
     QTestState *qts = global_qtest;
     uint64_t blk_per_4k = BLOCK_SIZE / 512;
     uint64_t i;
 
    g_assert_cmpuint(BLOCK_SIZE % 512, ==, 0);

    for (i = 0; i < load_ops; i++) {
         QVirtioBlkReq req;
         uint64_t addr;
         uint32_t free_head;
         uint8_t status;
         char *buf = g_malloc0(BLOCK_SIZE);
 
         /* 简单写入一些内容，便于后续 debug（可选） */
         memset(buf, (int)(i & 0xff), BLOCK_SIZE);
 
         req.type = VIRTIO_BLK_T_OUT;
         req.ioprio = 1;
         req.sector = i * blk_per_4k;
         req.data = buf;
 
         addr = virtio_blk_build_request(alloc, dev, &req, BLOCK_SIZE);
         g_free(buf);
 
         /* 三段描述符布局：header(16) + data(4K) + status(1) */
         free_head = qvirtqueue_add(qts, vq, addr, 16, false, true);
         qvirtqueue_add(qts, vq, addr + 16, BLOCK_SIZE, false, true);
         qvirtqueue_add(qts, vq, addr + 16 + BLOCK_SIZE, 1, true, false);
 
         qvirtqueue_kick(qts, dev, vq, free_head);
         qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL,
                                QVIRTIO_BLK_TIMEOUT_US);
 
         status = readb(addr + 16 + BLOCK_SIZE);
         g_assert_cmpint(status, ==, 0);
 
         guest_free(alloc, addr);
     }
 
     g_print("[virtio-dma-bench] load phase done: %u blocks of %u bytes\n",
            (unsigned)load_ops, (unsigned)BLOCK_SIZE);
 }
 
/* run 阶段：随机读写 run_ops 次，记录简单统计（受队列容量限制） */
 static void virtio_blk_run_phase(QVirtioDevice *dev,
                                  QGuestAllocator *alloc,
                                 QVirtQueue *vq,
                                 uint64_t run_ops)
 {
     QTestState *qts = global_qtest;
     uint64_t blk_per_4k = BLOCK_SIZE / 512;
     uint64_t i;
     uint64_t total_lat_ns = 0;
     uint64_t max_lat_ns = 0;
 
     g_assert_cmpuint(BLOCK_SIZE % 512, ==, 0);
 
     /* 简单的 pseudo-random：使用 g_test_rand_* 或 rand_r 都可以 */
     GRand *rnd = g_rand_new_with_seed(0x12345678);
 
    for (i = 0; i < run_ops; i++) {
         QVirtioBlkReq req;
         uint64_t addr;
         uint32_t free_head;
         uint8_t status;
         uint64_t start_ns, end_ns, lat_ns;
         char *buf = g_malloc0(BLOCK_SIZE);
 
         /* 50% 写、50% 读 */
         bool is_write = (g_rand_int_range(rnd, 0, 100) < RUN_WRITE_RATIO);
         uint64_t block_index = g_rand_int_range(rnd, 0, LOAD_BLOCKS);
 
         if (is_write) {
             memset(buf, (int)(block_index & 0xff), BLOCK_SIZE);
             req.type = VIRTIO_BLK_T_OUT;
         } else {
             req.type = VIRTIO_BLK_T_IN;
         }
 
         req.ioprio = 1;
         req.sector = block_index * blk_per_4k;
         req.data = buf;
 
         addr = virtio_blk_build_request(alloc, dev, &req, BLOCK_SIZE);
         g_free(buf);
 
         free_head = qvirtqueue_add(qts, vq, addr, 16, false, true);
         /* 读请求 data 段是 write=true，写请求是 write=false */
         qvirtqueue_add(qts, vq, addr + 16, BLOCK_SIZE,
                        !is_write /* read into guest? */, true);
         qvirtqueue_add(qts, vq, addr + 16 + BLOCK_SIZE, 1, true, false);
 
         start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
         qvirtqueue_kick(qts, dev, vq, free_head);
         qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL,
                                QVIRTIO_BLK_TIMEOUT_US);
         end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
 
         lat_ns = end_ns - start_ns;
         total_lat_ns += lat_ns;
         if (lat_ns > max_lat_ns) {
             max_lat_ns = lat_ns;
         }
 
         status = readb(addr + 16 + BLOCK_SIZE);
         g_assert_cmpint(status, ==, 0);
 
         guest_free(alloc, addr);
 
        if ((i + 1) % 1000 == 0) {
             g_print("[virtio-dma-bench] run progress: %" G_GUINT64_FORMAT
                    "/%" G_GUINT64_FORMAT " ops\n",
                    i + 1, run_ops);
         }
     }
 
     g_rand_free(rnd);
 
     g_print("[virtio-dma-bench] run phase done: %" G_GUINT64_FORMAT
             " ops, avg_lat_ns=%" G_GUINT64_FORMAT ", max_lat_ns=%"
             G_GUINT64_FORMAT "\n",
            run_ops,
            run_ops ? (total_lat_ns / run_ops) : 0,
             max_lat_ns);
 }
 
/* 创建/销毁临时磁盘镜像，基本照抄 virtio-blk-test.c 的驱动准备逻辑 */
static void dma_drive_destroy(void *path)
{
    unlink(path);
    g_free(path);
    qos_invalidate_command_line();
}

static char *dma_drive_create(void)
{
    int fd, ret;
    char *t_path;

    /* 创建一个临时 raw 镜像文件 */
    fd = g_file_open_tmp("qtest.XXXXXX", &t_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    ret = ftruncate(fd, TEST_IMAGE_SIZE);
    g_assert_cmpint(ret, ==, 0);
    close(fd);

    g_test_queue_destroy(dma_drive_destroy, t_path);
    return t_path;
}

/*
 * 在生成 QEMU 命令行时追加 drive0 定义：
 *   -drive if=none,id=drive0,file=<tmp>,format=raw,auto-read-only=off
 * 这样 virtio-blk-pci,drive=drive0 才能找到对应的后端。
 */
static void *virtio_dma_blk_setup(GString *cmd_line, void *arg)
{
    char *tmp_path = dma_drive_create();

    g_string_append_printf(cmd_line,
                           " -drive if=none,id=drive0,file=%s,"
                           "format=raw,auto-read-only=off ",
                           tmp_path);

    return arg;
}

/* qgraph 测试入口：obj 是 QVirtioBlk*，alloc 是 guest allocator */
 static void virtio_dma_bench(void *obj, void *data, QGuestAllocator *t_alloc)
 {
     QVirtioBlk *blk_if = obj;
     QVirtioDevice *dev = blk_if->vdev;
     QVirtQueue *vq;
 
    vq = virtio_blk_setup_queues(dev, t_alloc);

    /*
     * libqos 的 qvirtqueue_add() 不会回收描述符，因此同一个 virtqueue
     * 在整个测试过程中可用的 descriptor 总数为 vq->size。每个请求使用
     * 3 个 descriptor（header + data + status），因此总请求数必须满足：
     *
     *   3 * (load_ops + run_ops) <= vq->size
     *
     * 这里根据用户期望的 LOAD_BLOCKS / RUN_OPS 与队列容量计算“实际”
     * load_ops / run_ops，避免出现 “Guest says index XXX is available” 错误。
     */
    uint64_t max_reqs_total = vq->size / 3;
    uint64_t load_ops = MIN((uint64_t)LOAD_BLOCKS, max_reqs_total / 2);
    uint64_t run_ops  = MIN((uint64_t)RUN_OPS, max_reqs_total - load_ops);

    g_print("[virtio-dma-bench] vq.size=%u, max_reqs_total=%" G_GUINT64_FORMAT
            ", load_ops=%" G_GUINT64_FORMAT ", run_ops=%" G_GUINT64_FORMAT "\n",
            vq->size, max_reqs_total, load_ops, run_ops);

    /* load 阶段 */
    if (load_ops > 0) {
        virtio_blk_load_phase(dev, t_alloc, vq, load_ops);
    }

    /* run 阶段 */
    if (run_ops > 0) {
        virtio_blk_run_phase(dev, t_alloc, vq, run_ops);
    }
 
     qvirtqueue_cleanup(dev->bus, vq, t_alloc);
 }
 
 /* 为 qgraph 注册测试 */
 static void register_virtio_dma_bench(void)
 {
     QOSGraphTestOptions opts = {
        .before = virtio_dma_blk_setup,
     };
 
     /* 这个名字会出现在 meson test --list 里 */
     qos_add_test("virtio-dma/blk-bench", "virtio-blk",
                  virtio_dma_bench, &opts);
 }
 
 libqos_init(register_virtio_dma_bench);