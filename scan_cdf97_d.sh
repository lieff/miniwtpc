#!/bin/bash
# scan_cdf97_d.sh - Grid search for optimal CDF97_D wavelet constant
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Configuration
IMGDIR="${1:-$SCRIPT_DIR/images}"
TMPD="/tmp/wtpc_dscan"
OUT_CSV="${2:-/tmp/cdf97_d_scan.csv}"
NTARGETS=(400 800 1000 1400 2000 4000 8000 16000)

# D range: around the candidate peak 0.48
DSTART="${3:-0.470}"
DEND="${4:-0.490}"
DSTEP="${5:-0.001}"

rm -rf "$TMPD"
mkdir -p "$TMPD"

# Collect image list (also include root .png if images/ dir)
find "$IMGDIR" -maxdepth 1 -name "*.png" -type f 2>/dev/null | sort > "$TMPD/imgs.txt"
# Also include .png files from project root if different from IMGDIR
if [ "$IMGDIR" != "$SCRIPT_DIR" ]; then
    find "$SCRIPT_DIR" -maxdepth 1 -name "*.png" -type f 2>/dev/null | sort >> "$TMPD/imgs.txt"
fi
sort -u "$TMPD/imgs.txt" -o "$TMPD/imgs.txt"
nimg=$(wc -l < "$TMPD/imgs.txt")
echo "Images: $nimg"
echo "D range: $DSTART .. $DEND step $DSTEP"
echo "Targets: ${NTARGETS[*]}"

# Count total (D,target) combinations
nd=$(echo "scale=0; ($DEND - $DSTART) / $DSTEP + 1" | bc -l 2>/dev/null || echo 21)
nt=${#NTARGETS[@]}
total_combos=$((nd * nt))

# For each D value and each target, encode all images at matching size
echo "D,target,avg_ssim2,avg_size,count" > "$OUT_CSV"

combo=0
report() { local cur=$1; printf "\r[%3d/%3d] scanning..." "$cur" "$total_combos"; }

D=$DSTART
while [ "$(echo "$D <= $DEND" | bc -l 2>/dev/null)" = "1" ]; do
    for target in "${NTARGETS[@]}"; do
        combo=$((combo + 1))
        report "$combo"
        sum_ssim2=0; sum_size=0; count=0
        i=0
        while read img; do
            [ -z "$img" ] && continue
            i=$((i + 1))
            # Encode with given D and target size
            out="$TMPD/enc_${RANDOM}.wtpc"
            dec="$TMPD/dec.png"
            WTPC_CDF97_D="$D" ./wtpc -e "$img" -o "$out" -b "$target" -m ebcot >/dev/null 2>&1
            sz=$(stat -c%s "$out" 2>/dev/null || echo 0)
            WTPC_CDF97_D="$D" ./wtpc -d "$out" -o "$dec" >/dev/null 2>&1
            ssim2=$(ssimulacra2 "$img" "$dec" 2>/dev/null || echo "skip")
            if [ "$ssim2" = "skip" ] || [ "$ssim2" = "0" ] && [ "$sz" -eq 0 ]; then
                rm -f "$out" "$dec"; continue
            fi
            sum_ssim2=$(echo "$sum_ssim2 + $ssim2" | bc -l)
            sum_size=$((sum_size + sz))
            count=$((count + 1))
            rm -f "$out" "$dec"
        done < "$TMPD/imgs.txt"
        
        avg_ssim2=$(echo "scale=6; $sum_ssim2 / $count" | bc -l 2>/dev/null || echo 0)
        avg_size=$((sum_size / count))
        echo "$D,$target,$avg_ssim2,$avg_size,$count" >> "$OUT_CSV"
    done
    D=$(echo "scale=6; $D + $DSTEP" | bc -l)
done

printf "\r%-50s\n" "Done. $(wc -l < "$OUT_CSV" | tr -d ' ') rows written."

echo ""
echo "=== Results: $OUT_CSV ==="
echo ""
echo "Best D per target:"
python3 -c "
import csv
from collections import defaultdict
best = {}
with open('$OUT_CSV') as f:
    for row in csv.DictReader(f):
        d = float(row['D']); t = int(row['target']); s = float(row['avg_ssim2'])
        k = (t,)
        if k not in best or s > best[k][1]: best[k] = (d, s)
print(f'{\"Target\":<8} {\"Best D\":>8}  {\"ssim2\":>8}')
for t in sorted(set(k[0] for k in best)):
    d, s = best[(t,)]
    print(f'{t:<8} {d:>8.4f}  {s:>8.3f}')
"
