#!/usr/bin/env python3
import re
import sys
import argparse
import os
from collections import defaultdict

LINE_RE = re.compile(
    r'\[virtio-dma\]\s+dir=(?P<dir>\S+)\s+dev=(?P<dev>\S+)\s+'
    r'gpa=0x(?P<gpa>[0-9a-fA-F]+)\s+'
    r'hva=(?P<hva>0x[0-9a-fA-F]+)\s+'
    r'len=(?P<len>\d+)\s+'
    r'touch_ns=(?P<touch_ns>-?\d+)'
)

def parse_args():
    p = argparse.ArgumentParser(
        description="Analyze virtio DMA HVA page touch heat from /tmp/virtio_dma.log"
    )
    p.add_argument("logfile", help="Path to virtio_dma.log (e.g. /tmp/virtio_dma.log)")
    p.add_argument("--page-size", type=int, default=None,
                   help="Host page size in bytes (default: use os.sysconf)")
    p.add_argument("--start-ns", type=int, default=None,
                   help="Only count entries with touch_ns >= this (ns, relative value)")
    p.add_argument("--end-ns", type=int, default=None,
                   help="Only count entries with touch_ns <= this (ns, relative value)")
    p.add_argument("--top", type=int, default=20,
                   help="Number of hottest pages to show")
    p.add_argument("--sort-by", choices=["count", "total", "avg"], default="count",
                   help="Sort by: count, total (total touch_ns), avg (avg touch_ns)")
    return p.parse_args()

def main():
    args = parse_args()

    if args.page_size is None:
        page_size = os.sysconf("SC_PAGE_SIZE")
    else:
        page_size = args.page_size

    print(f"Using host page size = {page_size} bytes", file=sys.stderr)

    # key = HVA page base address (int), value = dict with stats
    stats = defaultdict(lambda: {"count": 0, "total_ns": 0, "min_ns": None, "max_ns": None})

    with open(args.logfile, "r") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue

            hva_str = m.group("hva")
            touch_ns = int(m.group("touch_ns"))

            # 按需做简单的时间窗口过滤（这里 touch_ns 是单次耗时；你也可以换成其他时间字段）
            if args.start_ns is not None and touch_ns < args.start_ns:
                continue
            if args.end_ns is not None and touch_ns > args.end_ns:
                continue

            hva = int(hva_str, 16)
            hva_page = hva & ~(page_size - 1)

            s = stats[hva_page]
            s["count"] += 1
            s["total_ns"] += touch_ns
            s["min_ns"] = touch_ns if s["min_ns"] is None else min(s["min_ns"], touch_ns)
            s["max_ns"] = touch_ns if s["max_ns"] is None else max(s["max_ns"], touch_ns)

    # 计算平均值并整理成列表
    rows = []
    for page, s in stats.items():
        if s["count"] == 0:
            continue
        avg = s["total_ns"] / s["count"]
        rows.append((page, s["count"], s["total_ns"], avg, s["min_ns"], s["max_ns"]))

    if not rows:
        print("No matching entries found.", file=sys.stderr)
        return

    # 排序依据
    if args.sort_by == "count":
        rows.sort(key=lambda x: x[1], reverse=True)
    elif args.sort_by == "total":
        rows.sort(key=lambda x: x[2], reverse=True)
    elif args.sort_by == "avg":
        rows.sort(key=lambda x: x[3], reverse=True)

    print(f"{'HVA_PAGE':>18}  {'COUNT':>8}  {'TOTAL_NS':>12}  {'AVG_NS':>12}  {'MIN_NS':>12}  {'MAX_NS':>12}")
    print("-" * 80)
    for page, cnt, total_ns, avg_ns, min_ns, max_ns in rows[:args.top]:
        print(f"0x{page:016x}  {cnt:8d}  {total_ns:12d}  {int(avg_ns):12d}  {min_ns:12d}  {max_ns:12d}")

if __name__ == "__main__":
    main()
