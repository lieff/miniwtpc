#!/bin/bash
# scan_block_size.sh - Find optimal subband block-scan size per quality level
# Usage: ./scan_block_size.sh <image_dir> [output_csv] [-n N]
#   -n N   limit to N random images (default: all)

IMGDIR="${1:-images}"
OUT_CSV="${2:-/tmp/block_scan.csv}"
TMPD="/tmp/wtpc_blockscan"
NIMG=""

shift; shift 2>/dev/null  # skip IMGDIR and OUT_CSV
while [ $# -gt 0 ]; do
  case "$1" in
    -n) NIMG="$2"; shift 2;;
    *) shift;;
  esac
done

rm -rf "$TMPD"
mkdir -p "$TMPD"

# Collect image list
find "$IMGDIR" -maxdepth 1 -name "*.png" -type f 2>/dev/null | shuf > "$TMPD/imgs.txt"
total=$(wc -l < "$TMPD/imgs.txt")
if [ -n "$NIMG" ] && [ "$NIMG" -lt "$total" ]; then
  head -n "$NIMG" "$TMPD/imgs.txt" > "$TMPD/imgs_sub.txt"
  mv "$TMPD/imgs_sub.txt" "$TMPD/imgs.txt"
fi
nimg=$(wc -l < "$TMPD/imgs.txt")
if [ "$nimg" -eq 0 ]; then echo "No images found in $IMGDIR"; exit 1; fi
echo "Images: $nimg / $total"
echo ""

# Quality levels + midpoints for finer granularity
Q_LIST=(
  1 10 20 50 80 109 138 194 250 275 300 330
  360 370 380 390 400 410 420 430 440 450 460 470 480 490 500 510 520 530 540 550 560 570
  580 600 620 650 680 730 780 810 840 870 900 930 960 992 1024
)
BLOCK_SIZES=(0 2 4 8 16)   # WTPC_SB_BLK values: 0=linear, 2/4/8/16=block

echo "q,lin,2x2,4x4,8x8,16x16,best" > "$OUT_CSV"

printf -- "%-6s | %-8s | %-8s | %-8s | %-8s | %-8s | %-6s\n" "q" "lin" "2x2" "4x4" "8x8" "16x16" "best"
printf -- "-------|----------|----------|----------|----------|----------|--------\n"

for q in "${Q_LIST[@]}"; do
  printf -- "%-6s |" "q=$q"
  best_sz=999999999; best_name="?"
  s0=0; s2=0; s4=0; s8=0; s16=0
  for bs in "${BLOCK_SIZES[@]}"; do
    sum=0
    while read img; do
      [ -z "$img" ] && continue
      WTPC_SB_BLK=$bs ./wtpc -e "$img" -o "$TMPD/enc.wtp" -m huffman -q "$q" >/dev/null 2>&1
      sz=$(stat -c%s "$TMPD/enc.wtp" 2>/dev/null)
      sum=$((sum + ${sz:-0}))
    done < "$TMPD/imgs.txt"
    avg=$((sum / nimg))
    case $bs in 0) s0=$avg;; 2) s2=$avg;; 4) s4=$avg;; 8) s8=$avg;; 16) s16=$avg;; esac
    printf -- " %-8d |" $avg
    [ $avg -lt $best_sz ] && best_sz=$avg && best_name=$([ $bs -eq 0 ] && echo "lin" || echo "${bs}x${bs}")
  done
  printf -- " %-6s\n" "$best_name"
  echo "$q,$s0,$s2,$s4,$s8,$s16,$best_name" >> "$OUT_CSV"
done

echo ""
echo "Results: $OUT_CSV"
