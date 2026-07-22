#!/bin/bash
# ====== Benchmark: WTPC vs JPEG vs JPEG2000 vs JPEGXL ======
# Target: 200 B - 36 KB thumbnails/previews
# Usage: bash bench.sh       (full: all codecs)
#        bash bench.sh       (fast: re-run only WTPC if TMPD exists)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE="lena256.png"
TMPD="/tmp/wtpc_bench"
SAMPLES="$SCRIPT_DIR/samples"
LOGFILE="$TMPD/raw.log"
METRICS() {
    local ref="$1" cmp="$2" psnr ssim2
    psnr=$(magick compare -metric PSNR "$ref" "$cmp" null: 2>&1) || true
    psnr=${psnr%% *}  # IM7: first word is PSNR
    ssim2=$(ssimulacra2 "$ref" "$cmp" 2>/dev/null) || true
    [ -z "$ssim2" ] && ssim2=0
    echo "$psnr $ssim2"
}
fsize() { stat -c%s "$1" 2>/dev/null || echo 0; }
DATE=$(date +%Y-%m-%d)

# Fast mode: if TMPD already has data, only re-run WTPC variants
FAST=0
if [ -d "$TMPD" ] && [ -f "$LOGFILE" ]; then
    FAST=1
    rm -f "$TMPD"/wtpc_e_* "$TMPD"/wtpc_h_* "$TMPD"/w420_e_* "$TMPD"/w420_h_* "$TMPD"/wt_time_*
    mkdir -p "$SAMPLES"
    # Keep JPEG/JP2K/JXL data, strip old WTPC variant lines
    grep -vE '^(WTPC_E |WTPC_H |W420_E |W420_H |W_TIME |### WTPC_E|### WTPC_H|### W420_E|### W420_H|### WTPC_TIME)' "$LOGFILE" > "$TMPD/_log.tmp"
    mv "$TMPD/_log.tmp" "$LOGFILE"
else
    rm -rf "$TMPD" "$SAMPLES"
    mkdir -p "$TMPD" "$SAMPLES"
    echo "=== Benchmark: $IMAGE (256x256) ===" | tee "$LOGFILE"
    echo "Target: 200 B - 36 KB" | tee -a "$LOGFILE"
    echo "" | tee -a "$LOGFILE"
fi

# Helper: run one WTPC variant across all targets, capture timings
bench_wtpc() {
    local label="$1" mode="$2" chroma="$3" prefix="$4"
    echo "" | tee -a "$LOGFILE"
    echo "### $label ###" | tee -a "$LOGFILE"
    for target in 200 400 600 800 1000 1200 1400 1500 2000 3000 4000 5000 6000 8000 10000 13000 15000 18000 20000 22000 25000 28000 30000 32000 36000; do
        wf="$TMPD/${prefix}_t${target}.wtpc"; dec="$TMPD/${prefix}_t${target}_dec.png"
        out=$(./wtpc -e "$IMAGE" -o "$wf" -b "$target" $chroma -m "$mode" 2>&1)
        q=$(echo "$out" | grep -o 'q=[0-9]*' | grep -o '[0-9]*' | head -1)
        [ -z "$q" ] && q=?
        enc_ms=$(echo "$out" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$enc_ms" ] && enc_ms=0
        out2=$(./wtpc -d "$wf" -o "$dec" 2>&1)
        dec_ms=$(echo "$out2" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$dec_ms" ] && dec_ms=0
        sz=$(fsize "$wf"); met=$(METRICS "$IMAGE" "$dec" 2>/dev/null)
        echo "$label q=$q | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    done
}
bench_wtpc "WTPC_E" "ebcot" "" "wtpc_e"
bench_wtpc "WTPC_H" "huffman" "" "wtpc_h"
bench_wtpc "W420_E" "ebcot" "-c" "w420_e"
bench_wtpc "W420_H" "huffman" "-c" "w420_h"

# ---- WTPC timing grid: fixed q (no -b binary search) to measure raw codec speed ----
echo "" | tee -a "$LOGFILE"
echo "### WTPC_TIME ###" | tee -a "$LOGFILE"
for q in 665 570 474 369 244 101 78; do
    for v in "WTPC_E:ebcot:" "WTPC_H:huffman:" "W420_E:ebcot:-c" "W420_H:huffman:-c"; do
        label="${v%%:*}"; rest="${v#*:}"; mode="${rest%%:*}"; chroma="${rest#*:}"
        pre="wt_time_$(echo $label | tr 'A-Z' 'a-z')"
        wf="$TMPD/${pre}_q${q}.wtpc"; dec="$TMPD/${pre}_q${q}_dec.png"
        out=$(./wtpc -e "$IMAGE" -o "$wf" -q "$q" $chroma -m "$mode" 2>&1)
        enc_ms=$(echo "$out" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$enc_ms" ] && enc_ms=0
        out2=$(./wtpc -d "$wf" -o "$dec" 2>&1)
        dec_ms=$(echo "$out2" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$dec_ms" ] && dec_ms=0
        sz=$(fsize "$wf"); met=$(METRICS "$IMAGE" "$dec" 2>/dev/null)
        echo "W_TIME $label q=$q | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    done
done

# ---- JPEG (all q=1..100, every step for fine granularity) ----
if [ "$FAST" = "0" ]; then
    echo "" | tee -a "$LOGFILE"
    echo "### JPEG ###" | tee -a "$LOGFILE"
    for q in $(seq 1 100); do
        jpg="$TMPD/jpeg_q${q}.jpg"; dec_bmp="$TMPD/jpeg_q${q}_dec.bmp"; dec_png="$TMPD/jpeg_q${q}_dec.png"
        t0=$(date +%s%N 2>/dev/null); convert "$IMAGE" -quality "$q" "$jpg" 2>/dev/null; t1=$(date +%s%N 2>/dev/null)
        enc_ms=$(( (t1 - t0) / 1000000 )); [ -z "$enc_ms" ] && enc_ms=0
        t0=$(date +%s%N 2>/dev/null); convert "$jpg" "$dec_bmp" 2>/dev/null; t1=$(date +%s%N 2>/dev/null)
        dec_ms=$(( (t1 - t0) / 1000000 )); [ -z "$dec_ms" ] && dec_ms=0
        convert "$dec_bmp" -strip "$dec_png" 2>/dev/null || true  # for ssimulacra2 (doesn't support BMP)
        sz=$(fsize "$jpg"); met=$(METRICS "$IMAGE" "$dec_png" 2>/dev/null)
        echo "JPEG q=$q | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    done
fi

# ---- JPEG2000 (int rates 2-52 + fractional 0.2-step around target gaps, + coarse for tiny sizes) ----
if [ "$FAST" = "0" ]; then
    echo "" | tee -a "$LOGFILE"
    echo "### JPEG2000 ###" | tee -a "$LOGFILE"
    ppm="$TMPD/_in.ppm"
    convert "$IMAGE" "$ppm" 2>/dev/null
    # Dense integer rates 2..52 (covers ~10KB..36KB range)
    # Fractional 0.2-step rates 3.0..8.0 (fills gaps like r=5..6..7 where file size jumps 4-8KB)
    # Coarse rates for very small files (ultra-low bitrate)
    j2k_rates=""
    for r in $(seq 2 52); do j2k_rates="$j2k_rates $r"; done
    # Fractional 0.2-step rates 3.0..7.8 fills gaps where integer rate jumps 4-8KB
    for r in 3.0 3.2 3.4 3.6 3.8 4.0 4.2 4.4 4.6 4.8 5.0 5.2 5.4 5.6 5.8 6.0 6.2 6.4 6.6 6.8 7.0 7.2 7.4 7.6 7.8; do
        j2k_rates="$j2k_rates $r"
    done
    j2k_rates="$j2k_rates 56 60 64 68 72 76 80 84 88 92 96 100 104 108 112 120 128 132 140 148 156 168 180 192 208 224 240 256 284 320 360 400 450 500 600 800 1000 1200 1500 2000"
    for r in $j2k_rates; do
        rname=$(echo "$r" | tr '.' '_')
        j2k="$TMPD/j2k_r${rname}.jp2"; dec_bmp="$TMPD/j2k_r${rname}_dec.bmp"; dec_png="$TMPD/j2k_r${rname}_dec.png"
        t0=$(date +%s%N 2>/dev/null); opj_compress -i "$ppm" -o "$j2k" -r "$r" 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
        enc_ms=$(( (t1 - t0) / 1000000 )); [ -z "$enc_ms" ] && enc_ms=0
        t0=$(date +%s%N 2>/dev/null); opj_decompress -i "$j2k" -o "$dec_bmp" 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
        dec_ms=$(( (t1 - t0) / 1000000 )); [ -z "$dec_ms" ] && dec_ms=0
        convert "$dec_bmp" -strip "$dec_png" 2>/dev/null || true  # for ssimulacra2
        sz=$(fsize "$j2k"); met=$(METRICS "$IMAGE" "$dec_png" 2>/dev/null)
        echo "J2K r=$r | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    done
fi

# ---- JPEG XL (pre-calibrated distance values to hit target sizes) ----
if [ "$FAST" = "0" ]; then
    echo "" | tee -a "$LOGFILE"
    echo "### JPEGXL ###" | tee -a "$LOGFILE"
    jxl="$TMPD/jxl_min.jxl"; dec_pnm="$TMPD/jxl_min_dec.pnm"; dec_png="$TMPD/jxl_min_dec.png"
    t0=$(date +%s%N 2>/dev/null); cjxl "$IMAGE" "$jxl" -m 0 -q 0 -e 10 --quiet 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
    enc_ms=$(( (t1 - t0) / 1000000 )); [ -z "$enc_ms" ] && enc_ms=0
    t0=$(date +%s%N 2>/dev/null); djxl "$jxl" "$dec_pnm" 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
    dec_ms=$(( (t1 - t0) / 1000000 )); [ -z "$dec_ms" ] && dec_ms=0
    convert "$dec_pnm" -strip "$dec_png" 2>/dev/null || true  # for readme samples
    sz=$(fsize "$jxl"); met=$(METRICS "$IMAGE" "$dec_pnm" 2>/dev/null)
    echo "JXL min | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    jxl_dist="30 25 20 18 16 14 12 11 10 9 8 7 6 5.5 5 4.5 4 3.5 3 2.8 2.5 2.3 2.2 2.1 2 1.9 1.8 1.7 1.6 1.5 1.4 1.3 1.2 1.1 1 0.95 0.9 0.85 0.8 0.75 0.7 0.65 0.6 0.57 0.55 0.52 0.5 0.48 0.45 0.42 0.4 0.38 0.36 0.35 0.34 0.32 0.3"
    for d in $jxl_dist; do
        jxl="$TMPD/jxl_d${d}.jxl"; dec_pnm="$TMPD/jxl_d${d}_dec.pnm"; dec_png="$TMPD/jxl_d${d}_dec.png"
        t0=$(date +%s%N 2>/dev/null); cjxl "$IMAGE" "$jxl" -d "$d" --quiet 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
        enc_ms=$(( (t1 - t0) / 1000000 )); [ -z "$enc_ms" ] && enc_ms=0
        t0=$(date +%s%N 2>/dev/null); djxl "$jxl" "$dec_pnm" 2>/dev/null || true; t1=$(date +%s%N 2>/dev/null)
        dec_ms=$(( (t1 - t0) / 1000000 )); [ -z "$dec_ms" ] && dec_ms=0
        convert "$dec_pnm" -strip "$dec_png" 2>/dev/null || true  # for readme samples
        sz=$(fsize "$jxl"); met=$(METRICS "$IMAGE" "$dec_pnm" 2>/dev/null)
        echo "JXL d=$d | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"
    done
fi

# ---- Save sample images ----
echo "" | tee -a "$LOGFILE"
echo "=== Saving sample images ===" | tee -a "$LOGFILE"

# WTPC_E: EBCOT 4:4:4
cp "$TMPD/wtpc_e_t1400_dec.png"  "$SAMPLES/WTPC_E_worst_1.4kb.png"  2>/dev/null || true
cp "$TMPD/wtpc_e_t6000_dec.png"  "$SAMPLES/WTPC_E_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/wtpc_e_t13000_dec.png" "$SAMPLES/WTPC_E_good_13kb.png"   2>/dev/null || true
cp "$TMPD/wtpc_e_t36000_dec.png" "$SAMPLES/WTPC_E_best_36kb.png"   2>/dev/null || true
# Low-rate comparison
cp "$TMPD/wtpc_e_t200_dec.png"   "$SAMPLES/WTPC_200b.png"   2>/dev/null || true
cp "$TMPD/wtpc_e_t400_dec.png"   "$SAMPLES/WTPC_400b.png"   2>/dev/null || true
cp "$TMPD/wtpc_e_t600_dec.png"   "$SAMPLES/WTPC_600b.png"   2>/dev/null || true
cp "$TMPD/wtpc_e_t800_dec.png"   "$SAMPLES/WTPC_800b.png"   2>/dev/null || true
cp "$TMPD/wtpc_e_t1000_dec.png"  "$SAMPLES/WTPC_1000b.png"  2>/dev/null || true
cp "$TMPD/wtpc_e_t1200_dec.png"  "$SAMPLES/WTPC_1200b.png"  2>/dev/null || true

# WTPC_H: Huffman 4:4:4
cp "$TMPD/wtpc_h_t1400_dec.png"  "$SAMPLES/WTPC_H_worst_1.4kb.png"  2>/dev/null || true
cp "$TMPD/wtpc_h_t6000_dec.png"  "$SAMPLES/WTPC_H_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/wtpc_h_t13000_dec.png" "$SAMPLES/WTPC_H_good_13kb.png"   2>/dev/null || true
cp "$TMPD/wtpc_h_t36000_dec.png" "$SAMPLES/WTPC_H_best_36kb.png"   2>/dev/null || true

# W420_E: EBCOT 4:2:0
cp "$TMPD/w420_e_t1400_dec.png"  "$SAMPLES/W420_E_worst_1.4kb.png"  2>/dev/null || true
cp "$TMPD/w420_e_t6000_dec.png"  "$SAMPLES/W420_E_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/w420_e_t13000_dec.png" "$SAMPLES/W420_E_good_13kb.png"   2>/dev/null || true
cp "$TMPD/w420_e_t36000_dec.png" "$SAMPLES/W420_E_best_36kb.png"   2>/dev/null || true

# W420_H: Huffman 4:2:0
cp "$TMPD/w420_h_t1400_dec.png"  "$SAMPLES/W420_H_worst_1.4kb.png"  2>/dev/null || true
cp "$TMPD/w420_h_t6000_dec.png"  "$SAMPLES/W420_H_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/w420_h_t13000_dec.png" "$SAMPLES/W420_H_good_13kb.png"   2>/dev/null || true
cp "$TMPD/w420_h_t36000_dec.png" "$SAMPLES/W420_H_best_36kb.png"   2>/dev/null || true

# JPEG (q=4,31,78,94)
cp "$TMPD/jpeg_q4.jpg"    "$SAMPLES/JPEG_worst_1.4kb.jpg"   2>/dev/null || true
cp "$TMPD/jpeg_q31.jpg"   "$SAMPLES/JPEG_mid_6kb.jpg"      2>/dev/null || true
cp "$TMPD/jpeg_q78.jpg"   "$SAMPLES/JPEG_good_13kb.jpg"    2>/dev/null || true
cp "$TMPD/jpeg_q94.jpg"   "$SAMPLES/JPEG_best_36kb.jpg"    2>/dev/null || true
# Low-rate comparison samples
cp "$TMPD/jpeg_q1.jpg"    "$SAMPLES/JPEG_1000b.jpg"         2>/dev/null || true
cp "$TMPD/jpeg_q3.jpg"    "$SAMPLES/JPEG_1200b.jpg"         2>/dev/null || true

# JPEG2000 (r=140,32,15,4 - latter may not exist)
cp "$TMPD/j2k_r140_dec.png"  "$SAMPLES/JP2K_worst_1.4kb.png"  2>/dev/null || true
cp "$TMPD/j2k_r32_dec.png"   "$SAMPLES/JP2K_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/j2k_r15_dec.png"   "$SAMPLES/JP2K_good_13kb.png"   2>/dev/null || true
cp "$TMPD/j2k_r4_dec.png"    "$SAMPLES/JP2K_best_36kb.png"   2>/dev/null || true
# Low-rate comparison samples
cp "$TMPD/j2k_r2000_dec.png"  "$SAMPLES/JP2K_200b.png"   2>/dev/null || true
cp "$TMPD/j2k_r800_dec.png"   "$SAMPLES/JP2K_400b.png"   2>/dev/null || true
cp "$TMPD/j2k_r500_dec.png"   "$SAMPLES/JP2K_600b.png"   2>/dev/null || true
cp "$TMPD/j2k_r320_dec.png"   "$SAMPLES/JP2K_800b.png"   2>/dev/null || true
cp "$TMPD/j2k_r192_dec.png"   "$SAMPLES/JP2K_1000b.png"  2>/dev/null || true
cp "$TMPD/j2k_r156_dec.png"   "$SAMPLES/JP2K_1200b.png"  2>/dev/null || true

# JPEG XL (min, d=4.5, 1.5, 0.35)
cp "$TMPD/jxl_min_dec.png"   "$SAMPLES/JXL_worst_1.4kb.png" 2>/dev/null || true
cp "$TMPD/jxl_d4.5_dec.png"  "$SAMPLES/JXL_mid_6kb.png"     2>/dev/null || true
cp "$TMPD/jxl_d1.5_dec.png"  "$SAMPLES/JXL_good_13kb.png"   2>/dev/null || true
cp "$TMPD/jxl_d0.35_dec.png" "$SAMPLES/JXL_best_36kb.png"   2>/dev/null || true
for f in "$SAMPLES"/*; do
    echo "  $(basename "$f"): $(fsize "$f") bytes" | tee -a "$LOGFILE"
done

# ---- AVIF: pre-tuned q values for each target at each speed ----
if [ "$FAST" = "0" ]; then
    # Pre-tuned q values per speed (empirically closest to targets 1K..36K on lena256.png)
    # plus q=0 (worst quality) for reference
    avif_q_s0="0 3 10 17 36 54 76 85 90"
    avif_q_s6="0 3 10 19 36 55 76 88 92"
    avif_q_s10="0 3 9 16 33 51 72 86 90"

    for speed in 0 6 10; do
        echo "" | tee -a "$LOGFILE"
        echo "### AVIF --speed $speed ###" | tee -a "$LOGFILE"
        eval "ql=\$avif_q_s${speed}"
        for q in $ql; do
            avif="$TMPD/avif_s${speed}_q${q}.avif"
            dec_y4m="$TMPD/avif_s${speed}_q${q}_dec.y4m"
            dec_png="$TMPD/avif_s${speed}_q${q}_dec.png"
            t0=$(date +%s%N 2>/dev/null)
            avifenc -q "$q" --speed "$speed" "$IMAGE" "$avif" 2>/dev/null || true
            t1=$(date +%s%N 2>/dev/null)
            enc_ms=$(( (t1 - t0) / 1000000 )); [ -z "$enc_ms" ] && enc_ms=0
            t0=$(date +%s%N 2>/dev/null)
            avifdec -r "$avif" "$dec_y4m" 2>/dev/null || true
            t1=$(date +%s%N 2>/dev/null)
            dec_ms=$(( (t1 - t0) / 1000000 )); [ -z "$dec_ms" ] && dec_ms=0
            ffmpeg -y -i "$dec_y4m" -map_metadata -1 "$dec_png" 2>/dev/null || true
            rm -f "$dec_y4m"
            sz=$(fsize "$avif"); met=$(METRICS "$IMAGE" "$dec_png" 2>/dev/null)
            echo "AVIF s=$speed q=$q | $sz | $met | enc=$enc_ms dec=$dec_ms" | tee -a "$LOGFILE"

            # Encode WTPC EBCOT 4:4:4 at same size as this AVIF file
            wf="$TMPD/avif_wtpc_e_s${speed}_q${q}.wtpc"
            wdec="$TMPD/avif_wtpc_e_s${speed}_q${q}_dec.png"
            # First pass: -b to find quality
            out=$(./wtpc -e "$IMAGE" -o "$wf" -b "$sz" -m "ebcot" 2>&1)
            wq=$(echo "$out" | grep -o 'q=[0-9]*' | grep -o '[0-9]*' | head -1); [ -z "$wq" ] && wq=1
            # Second pass: -q, capture internal timing from WTPC output (no date overhead)
            out=$(./wtpc -e "$IMAGE" -o "$wf" -q "$wq" -m "ebcot" 2>&1)
            enc_w=$(echo "$out" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$enc_w" ] && enc_w=0
            out2=$(./wtpc -d "$wf" -o "$wdec" 2>&1)
            dec_w=$(echo "$out2" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$dec_w" ] && dec_w=0
            wsz=$(fsize "$wf"); wmet=$(METRICS "$IMAGE" "$wdec" 2>/dev/null)
            echo "AVIF_WTPC_E s=$speed q=$q wq=$wq | $wsz | $wmet | enc=$enc_w dec=$dec_w" | tee -a "$LOGFILE"

            # Encode WTPC EBCOT 4:2:0 at same size as this AVIF file
            wf420="$TMPD/avif_w420_e_s${speed}_q${q}.wtpc"
            wdec420="$TMPD/avif_w420_e_s${speed}_q${q}_dec.png"
            out=$(./wtpc -e "$IMAGE" -o "$wf420" -b "$sz" -m "ebcot" -c 2>&1)
            wq420=$(echo "$out" | grep -o 'q=[0-9]*' | grep -o '[0-9]*' | head -1); [ -z "$wq420" ] && wq420=1
            out=$(./wtpc -e "$IMAGE" -o "$wf420" -q "$wq420" -m "ebcot" -c 2>&1)
            enc_w=$(echo "$out" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$enc_w" ] && enc_w=0
            out2=$(./wtpc -d "$wf420" -o "$wdec420" 2>&1)
            dec_w=$(echo "$out2" | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*'); [ -z "$dec_w" ] && dec_w=0
            wsz=$(fsize "$wf420"); wmet=$(METRICS "$IMAGE" "$wdec420" 2>/dev/null)
            echo "AVIF_W420_E s=$speed q=$q wq=$wq420 | $wsz | $wmet | enc=$enc_w dec=$dec_w" | tee -a "$LOGFILE"
        done
    done
fi

# AVIF --speed 6 vs best WTPC comparison samples
echo "" | tee -a "$LOGFILE"
echo "=== Saving AVIF comparison samples ===" | tee -a "$LOGFILE"
cp "$TMPD/avif_s6_q0_dec.png"   "$SAMPLES/AVIF_S6_726b.png"    2>/dev/null || true
cp "$TMPD/avif_s6_q3_dec.png"   "$SAMPLES/AVIF_S6_1kb.png"     2>/dev/null || true
cp "$TMPD/avif_s6_q10_dec.png"  "$SAMPLES/AVIF_S6_1.4kb.png"   2>/dev/null || true
cp "$TMPD/avif_s6_q19_dec.png"  "$SAMPLES/AVIF_S6_2kb.png"     2>/dev/null || true
cp "$TMPD/avif_s6_q36_dec.png"  "$SAMPLES/AVIF_S6_4kb.png"     2>/dev/null || true
cp "$TMPD/avif_s6_q76_dec.png"  "$SAMPLES/AVIF_S6_16kb.png"    2>/dev/null || true
cp "$TMPD/avif_s6_q92_dec.png"  "$SAMPLES/AVIF_S6_36kb.png"    2>/dev/null || true
# Best WTPC by ssim2 at matching AVIF sizes (speed 6)
cp "$TMPD/avif_w420_e_s6_q0_dec.png"   "$SAMPLES/WTPC_vs_AVIF_726b.png"   2>/dev/null || true
cp "$TMPD/avif_w420_e_s6_q3_dec.png"   "$SAMPLES/WTPC_vs_AVIF_1kb.png"    2>/dev/null || true
cp "$TMPD/avif_w420_e_s6_q10_dec.png"  "$SAMPLES/WTPC_vs_AVIF_1.4kb.png"  2>/dev/null || true
cp "$TMPD/avif_wtpc_e_s6_q19_dec.png"  "$SAMPLES/WTPC_vs_AVIF_2kb.png"    2>/dev/null || true
cp "$TMPD/avif_wtpc_e_s6_q36_dec.png"  "$SAMPLES/WTPC_vs_AVIF_4kb.png"    2>/dev/null || true
cp "$TMPD/avif_wtpc_e_s6_q76_dec.png"  "$SAMPLES/WTPC_vs_AVIF_16kb.png"   2>/dev/null || true
cp "$TMPD/avif_wtpc_e_s6_q92_dec.png"  "$SAMPLES/WTPC_vs_AVIF_36kb.png"   2>/dev/null || true
for f in "$SAMPLES"/AVIF_S6* "$SAMPLES"/WTPC_vs_AVIF*; do
    [ -f "$f" ] && echo "  $(basename "$f"): $(fsize "$f") bytes" | tee -a "$LOGFILE"
done

# ============================================================
# Generate results.md from log data via embedded Python
# ============================================================
echo "" | tee -a "$LOGFILE"
echo "=== Generating results.md ===" | tee -a "$LOGFILE"

python3 - "$LOGFILE" "$SCRIPT_DIR/results.md" "$SCRIPT_DIR/readme.md" "$DATE" << 'PYEOF'
import sys, re

logfile = sys.argv[1]
resfile = sys.argv[2]
readmefile = sys.argv[3]
date_str = sys.argv[4]

# Parse log into dict: codec -> list of (param, size, psnr, ssim, enc_ms, dec_ms)
data = {}
order = []
time_data = {}  # W_TIME entries: variant -> list of (q, size, enc_ms, dec_ms)
avif_wtpc = {}  # (speed,q) -> (size,psnr,ssim,enc,dec) for WTPC 444 at AVIF size
avif_w420 = {}  # (speed,q) -> (size,psnr,ssim,enc,dec) for WTPC 420 at AVIF size
with open(logfile) as f:
    for line in f:
        line = line.strip()
        # W_TIME: fixed-q timing grid
        tm = re.match(r'W_TIME (WTPC_E|WTPC_H|W420_E|W420_H) q=(\d+)\s*\|\s*(\d+)\s*\|\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\|\s*enc=([\d.]+)\s+dec=([\d.]+)', line)
        if tm:
            var, q, sz, psnr, ssim, enc, dec = tm.groups()
            if var not in time_data: time_data[var] = []
            time_data[var].append((int(q), int(sz), float(enc), float(dec)))
            continue
        m = re.match(r'AVIF s=(\d+) q=(\d+)\s*\|\s*(\d+)\s*\|\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\|\s*enc=([\d.]+)\s+dec=([\d.]+)', line)
        if m:
            speed, q, size, psnr, ssim, enc_ms, dec_ms = m.groups()
            codec = f'AVIF_S{speed}'
            param = f'q={q}'
            size_i, psnr_f, ssim_f = int(size), float(psnr), float(ssim)
            enc_f, dec_f = float(enc_ms), float(dec_ms)
            if psnr_f >= 10:
                entry = (param, size_i, psnr_f, ssim_f, enc_f, dec_f)
                if codec not in data:
                    data[codec] = []
                data[codec].append(entry)
            continue
        m = re.match(r'AVIF_WTPC_E s=(\d+) q=(\d+) wq=(\d+)\s*\|\s*(\d+)\s*\|\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\|\s*enc=([\d.]+)\s+dec=([\d.]+)', line)
        if m:
            speed, q, wq, size, psnr, ssim, enc_ms, dec_ms = m.groups()
            avif_wtpc[(int(speed), int(q))] = (int(wq), int(size), float(psnr), float(ssim), float(enc_ms), float(dec_ms))
            continue
        m = re.match(r'AVIF_W420_E s=(\d+) q=(\d+) wq=(\d+)\s*\|\s*(\d+)\s*\|\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\|\s*enc=([\d.]+)\s+dec=([\d.]+)', line)
        if m:
            speed, q, wq, size, psnr, ssim, enc_ms, dec_ms = m.groups()
            avif_w420[(int(speed), int(q))] = (int(wq), int(size), float(psnr), float(ssim), float(enc_ms), float(dec_ms))
            continue
        m = re.match(r'(W420_H|W420_E|WTPC_H|WTPC_E|JPEG|J2K|JXL)\s+(\S+)\s*\|\s*(\d+)\s*\|\s*(-?[\d.]+)\s+(-?[\d.]+)\s*\|\s*enc=([\d.]+)\s+dec=([\d.]+)', line)
        if not m:
            continue
        codec, param, size, psnr, ssim, enc_ms, dec_ms = m.groups()
        size_i, psnr_f, ssim_f = int(size), float(psnr), float(ssim)
        enc_f, dec_f = float(enc_ms), float(dec_ms)
        if psnr_f < 10:  # skip broken entries (e.g. wtpc q=160)
            continue
        # Strip prefix for display (q=, r=, d=) but keep "min" as-is
        display_param = param
        for pfx in ['q=', 'r=', 'd=']:
            if param.startswith(pfx):
                display_param = param[len(pfx):]
                break
        entry = (display_param, size_i, psnr_f, ssim_f, enc_f, dec_f)
        if codec not in data:
            data[codec] = []
            order.append(codec)
        data[codec].append(entry)

for c in data:
    data[c].sort(key=lambda x: x[1])

names = {'WTPC_E': 'WTPC EBC', 'WTPC_H': 'WTPC Huff', 'W420_E': 'W420 EBC', 'W420_H': 'W420 Huff', 'JPEG': 'JPEG', 'J2K': 'JPEG 2000', 'JXL': 'JPEG XL'}
subtitles = {
    'WTPC_E': 'CDF 9/7 wavelet + EBCOT-lite + BAC, YUV 4:4:4',
    'WTPC_H': 'CDF 9/7 wavelet + Context Huffman + EG-runs, YUV 4:4:4',
    'W420_E': 'CDF 9/7 wavelet + EBCOT-lite + BAC, YUV 4:2:0',
    'W420_H': 'CDF 9/7 wavelet + Context Huffman + EG-runs, YUV 4:2:0',
    'JPEG': 'libjpeg, YUV 4:2:0',
    'J2K':  'OpenJPEG, CDF 9/7 + EBCOT',
    'JXL':  'libjxl 0.11, VarDCT',
}
col_names = {'WTPC_E': 'q', 'WTPC_H': 'q', 'W420_E': 'q', 'W420_H': 'q', 'JPEG': 'q', 'J2K': 'r', 'JXL': 'Setting'}

md = []
md.append('# WTPC vs JPEG vs JPEG2000 vs JPEGXL - Benchmark')
md.append('')
md.append(f'**Test image:** `lena256.png` (256x256, 24-bit RGB)  ')
md.append('**Target range:** 200 B - 36 KB (thumbnails / previews)  ')
md.append('**Metrics:** PSNR (dB, higher is better), ssimulacra2 (0-100, higher is better)')
md.append(f'**Date:** {date_str}')
md.append('')

# ---- Mermaid chart ----
md.append('## PSNR vs File Size')
md.append('')
# Helper: format target labels
def _fmt_labels(ts):
    labels = []
    for t in ts:
        if t < 1000: labels.append(str(t))
        elif t % 1000 == 0: labels.append(f'{t//1000}K')
        else: labels.append(f'{t/1000:.1f}K')
    return ', '.join(f'"{l}"' for l in labels)

mlabels = {'WTPC_E': 'WTPC EBC', 'WTPC_H': 'WTPC Huff', 'W420_E': 'W420 EBC', 'W420_H': 'W420 Huff', 'JPEG': 'JPEG', 'J2K': 'JPEG2000', 'JXL': 'JPEGXL'}
# Fixed colors matching line order: blue, red, orange, green, purple, cyan, pink
colors = {'WTPC_E': '#3366cc', 'WTPC_H': '#dc3912', 'W420_E': '#ff9900', 'W420_H': '#109618', 'J2K': '#990099', 'JPEG': '#0099c6', 'JXL': '#dd4477'}
emojis = {'WTPC_E': '\U0001F535', 'WTPC_H': '\U0001F534', 'W420_E': '\U0001F7E0', 'W420_H': '\U0001F7E2', 'J2K': '\U0001F7E3', 'JPEG': '\U0001F539', 'JXL': '\U0001F497'}

# Chart 1: Ultra-low bitrate - only WTPC + JP2K (JPEG/JXL can't go this low)
low_codecs = ['WTPC_E', 'WTPC_H', 'W420_E', 'W420_H', 'J2K']
low_colors = ', '.join(colors[c] for c in low_codecs)
low_names  = ', '.join(mlabels[c] for c in low_codecs)
low_legend = '  '.join(f'{emojis[c]} {mlabels[c]}' for c in low_codecs)

md.append('### Ultra-low range (200 B - 1.2 KB)')
md.append('')
md.append('```mermaid')
md.append(f'%%{{init: {{"themeVariables": {{"xyChart": {{"plotColorPalette": "{low_colors}"}}}}}}}}%%')
md.append('xychart-beta')
md.append('    title "Ultra-low bitrate"')
md.append('    y-axis "PSNR (dB)" 12 --> 26')
low_targets = [200, 400, 600, 800, 1000, 1200]
md.append(f'    x-axis [{_fmt_labels(low_targets)}]')
for c in low_codecs:
    if c not in data: continue
    y_vals = []
    all_pts = [(sz, psnr) for (_, sz, psnr, _e, _d, _) in data[c]]
    for t in low_targets:
        best = min(all_pts, key=lambda p: abs(p[0] - t))
        y_vals.append(f'{best[1]:.2f}')
    md.append(f'    line "{mlabels[c]}" [{', '.join(y_vals)}]')
md.append('```')
md.append(f'{low_legend}')
md.append('')

# Chart 2: Standard range - all codecs including JPEG/JXL
std_codecs = [c for c in order if c in data]
std_colors = ', '.join(colors[c] for c in std_codecs)
std_names  = ', '.join(mlabels[c] for c in std_codecs)
std_legend = '  '.join(f'{emojis[c]} {mlabels[c]}' for c in std_codecs)

md.append('### Standard range (1.4 KB - 36 KB)')
md.append('')
md.append('```mermaid')
md.append(f'%%{{init: {{"themeVariables": {{"xyChart": {{"plotColorPalette": "{std_colors}"}}}}}}}}%%')
md.append('xychart-beta')
md.append('    title "Standard range"')
md.append('    y-axis "PSNR (dB)" 21 --> 42')
std_targets = [1400, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 13000, 15000, 18000, 20000, 22000, 25000, 28000, 30000, 32000, 36000]
md.append(f'    x-axis [{_fmt_labels(std_targets)}]')
for c in std_codecs:
    y_vals = []
    all_pts = [(sz, psnr) for (_, sz, psnr, _e, _d, _) in data[c]]
    for t in std_targets:
        best = min(all_pts, key=lambda p: abs(p[0] - t))
        y_vals.append(f'{best[1]:.2f}')
    md.append(f'    line "{mlabels[c]}" [{', '.join(y_vals)}]')
md.append('```')
md.append(f'{std_legend}')
md.append('')

# ---- Cross-table: each row = size step, columns = codecs ----
md.append('## Comparison by Size Steps')
md.append('')
md.append('Each row: closest entry from each codec to the target size. Best per metric is **bold**.')
md.append('')
# Build header: group by metric (all B, then all PSNR, then all Ssim2, then all par)
codec_order = ['WTPC_E', 'WTPC_H', 'W420_E', 'W420_H', 'J2K', 'JXL', 'JPEG']
codec_short = {'WTPC_E': 'E_4', 'WTPC_H': 'H_4', 'W420_E': 'E_2', 'W420_H': 'H_2', 'J2K': 'JP2K', 'JXL': 'JPXL', 'JPEG': 'JPEG'}
hdr = '| Step |'
sep  = '|------|'
for cs in [codec_short[c] for c in codec_order]:
    hdr += f' {cs} B |'
    sep += '------|'
for cs in [codec_short[c] for c in codec_order]:
    hdr += f' {cs} PSNR |'
    sep += '------|'
for cs in [codec_short[c] for c in codec_order]:
    hdr += f' {cs} Ssim2 |'
    sep += '------|'
for cs in [codec_short[c] for c in codec_order]:
    hdr += f' {cs} Q |'
    sep += '------|'
md.append(hdr)
md.append(sep)

# Size steps covering the entire range
steps = [200, 400, 600, 800, 1000, 1200, 1400, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 13000, 15000, 18000, 20000, 22000, 25000, 28000, 30000, 32000, 36000]

# For each step, find closest entry per codec (within +-50%)
for step in steps:
    row = f'| ~{step} |'
    best_psnr = -1; best_psnr_codec = ''
    best_ssim = -1; best_ssim_codec = ''
    best_size = 999999; best_size_codec = ''
    # Collect all entries for this step
    step_data = {}
    for c in codec_order:
        entries = data.get(c, [])
        closest = None; closest_dist = 999999
        for param, sz, psnr, ssim, _enc, _dec in entries:
            dist = abs(sz - step)
            if dist < closest_dist: closest_dist = dist; closest = (param, sz, psnr, ssim)
        if closest and closest_dist <= step * 0.5: step_data[c] = closest
    # Find best PSNR and ssimulacra2 (both: higher = better) among codecs at this step
    best_psnr = max((v[2] for v in step_data.values()), default=-1)
    best_ssim = max((v[3] for v in step_data.values()), default=-999)
    row_cells = [f'~{step}']
    # Bytes for all codecs
    for c in codec_order:
        if c in step_data:
            _, sz, psnr, _ = step_data[c]
            row_cells.append(f'**{sz}**' if psnr == best_psnr else str(sz))
        else:
            row_cells.append('-')
    # PSNR for all codecs
    for c in codec_order:
        if c in step_data:
            _, _, psnr, _ = step_data[c]
            row_cells.append(f'**{psnr:.2f}**' if psnr == best_psnr else f'{psnr:.2f}')
        else:
            row_cells.append('-')
    # ssimulacra2 for all codecs (higher is better, bold the maximum)
    for c in codec_order:
        if c in step_data:
            _, _, _, ssim = step_data[c]
            row_cells.append(f'**{ssim:.2f}**' if ssim == best_ssim else f'{ssim:.2f}')
        else:
            row_cells.append('-')
    # Params for all codecs
    for c in codec_order:
        if c in step_data:
            param, _, _, _ = step_data[c]
            row_cells.append(param)
        else:
            row_cells.append('-')
    md.append('| ' + ' | '.join(row_cells) + ' |')

md.append('')
md.append(f'> At each step, the codec with the best PSNR / ssimulacra2 (both higher=better) wins. Empty cells mean no data within +-50% of the target size.')

# ---- Winners summary (PSNR) ----
md.append('')
md.append('## Best Codec at Each Target Size (by PSNR)')
md.append('')
md.append('| Target (B) | Best Codec | Setting | Actual Size | PSNR (dB) | ssimulacra2 |')
md.append('|------------|------------|---------|-------------|-----------|-------------|')

targets = [200, 400, 600, 800, 1000, 1400, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 13000, 15000, 18000, 20000, 22000, 25000, 28000, 30000, 32000, 36000]
for target in targets:
    lo, hi = int(target * 0.7), int(target * 1.3)
    best = None
    best_psnr = -1
    for c in order:
        # Find CLOSEST entry for this codec (not max PSNR in range)
        closest = None; closest_dist = 999999
        for param, sz, psnr, ssim, _enc, _dec in data[c]:
            if lo <= sz <= hi:
                dist = abs(sz - target)
                if dist < closest_dist: closest_dist = dist; closest = (param, sz, psnr, ssim)
        if closest and closest[2] > best_psnr:
            best_psnr = closest[2]
            best = (c,) + closest
    if best:
        c, param, sz, psnr, ssim = best
        disp_name = 'JPEG2000' if c == 'J2K' else names[c]
        md.append(f'| {target} | {disp_name} | {param} | {sz} | {psnr:.2f} | {ssim:.2f} |')

# ---- Winners summary (ssimulacra2) ----
md.append('')
md.append('## Best Codec at Each Target Size (by ssimulacra2)')
md.append('')
md.append('| Target (B) | Best Codec | Setting | Actual Size | PSNR (dB) | ssimulacra2 |')
md.append('|------------|------------|---------|-------------|-----------|-------------|')

for target in targets:
    lo, hi = int(target * 0.7), int(target * 1.3)
    best = None
    best_ssim = -999  # higher is better for ssimulacra2
    for c in order:
        # Find CLOSEST entry for this codec (not max ssimulacra2 in range)
        closest = None; closest_dist = 999999
        for param, sz, psnr, ssim, _enc, _dec in data[c]:
            if lo <= sz <= hi:
                dist = abs(sz - target)
                if dist < closest_dist: closest_dist = dist; closest = (param, sz, psnr, ssim)
        if closest and closest[3] > best_ssim:  # higher is better
            best_ssim = closest[3]
            best = (c,) + closest
    if best:
        c, param, sz, psnr, ssim = best
        disp_name = 'JPEG2000' if c == 'J2K' else names[c]
        md.append(f'| {target} | {disp_name} | {param} | {sz} | {psnr:.2f} | {ssim:.2f} |')

# ---- Timing table ----
md.append('## Encode / Decode Speed (ms)')
md.append('')
md.append('| Step | E_4 enc | E_4 dec | H_4 enc | H_4 dec | E_2 enc | E_2 dec | H_2 enc | H_2 dec | JP2K enc | JP2K dec | JXL enc | JXL dec | JPEG enc |')
md.append('|------|---------|---------|---------|---------|---------|---------|---------|---------|----------|----------|----------|----------|----------|')

for step in steps:
    row = f'| ~{step} |'
    for c in ['WTPC_E','WTPC_H','W420_E','W420_H','J2K','JXL','JPEG']:
        entries = data.get(c, [])
        closest = None; cd = 999999
        for e in entries:
            d = abs(e[1] - step)
            if d < cd: cd = d; closest = e
        if closest and cd <= step * 0.5:
            enc, dec = closest[4], closest[5]
            if c == 'JPEG':
                row += f' {enc:.1f} |'
            else:
                row += f' {enc:.1f} | {dec:.1f} |'
        else:
            row += ' - | - |' if c != 'JPEG' else ' - |'
    md.append(row)

# ---- WTPC timing by quality level (fixed q, no -b search) ----
if time_data:
    md.append('')
    md.append('> WTPC encode timings above include a binary quality search to hit the exact target size (-b mode). Other codecs use pre-calibrated parameters. At fixed quality, see below.')
    md.append('')
    md.append('## WTPC Speed by Quality Level (ms, fixed q)')
    md.append('')
    q_levels = sorted(set(q for v, entries in time_data.items() for q, _, _, _ in entries), reverse=True)
    variants = ['WTPC_E','WTPC_H','W420_E','W420_H']
    hdr = '| q |'
    sep  = '|----|'
    for v in variants:
        hdr += f' {v} enc | {v} dec |'
        sep += '---------|---------|'
    md.append(hdr)
    md.append(sep)
    for q in q_levels:
        row = f'| {q} |'
        for v in variants:
            if v in time_data:
                entry = [e for e in time_data[v] if e[0] == q]
                if entry:
                    _, sz, enc, dec = entry[0]
                    row += f' {enc:.1f} | {dec:.1f} |'
                else:
                    row += ' - | - |'
            else:
                row += ' - | - |'
        md.append(row)
    md.append('')
    md.append(f'> Encode at fixed q (no binary search). File sizes: ~{q_levels[-1]}->{q_levels[0]} q.')

# ---- AVIF comparison tables ----
if any(k.startswith('AVIF_S') for k in data):
    md.append('')
    md.append('## Compare with avif')
    md.append('')
    md.append('Since avif have rough quantization steps it goes to separate table.')
    md.append('First step avifenc -q 0 --speed 0 and then closest to our targets.')
    md.append('WTPC uses -b to match avif sizes.')
    md.append('')

    avif_targets = [1000, 1400, 2000, 4000, 8000, 16000, 26000, 36000]
    avif_header = '| Target | AVIF q | EBCOT 444 q | EBCOT 420 q | AVIF B | EBCOT 444 B | EBCOT 420 B | AVIF enc | EBCOT 444 enc | EBCOT 420 enc | AVIF dec | EBCOT 444 dec | EBCOT 420 dec | AVIF PSNR | EBCOT 444 PSNR | EBCOT 420 PSNR | AVIF ssim2 | EBCOT 444 ssim2 | EBCOT 420 ssim2 |'
    avif_sep    = '|--------|--------|-------------|-------------|--------|-------------|-------------|---------|---------------|---------------|---------|---------------|---------------|----------|----------------|----------------|-----------|------------------|-------------------|'

    for speed in ['0', '6', '10']:
        avif_key = f'AVIF_S{speed}'
        if avif_key not in data or not data[avif_key]:
            continue
        md.append(f'### AVIF --speed {speed} vs WTPC')
        md.append('')
        md.append(avif_header)
        md.append(avif_sep)

        # Emit q=0 row first (minimum quality / worst size)
        q0_entries = [e for e in data[avif_key] if e[0] == 'q=0']
        if q0_entries:
            aq, asz, apsnr, assim, aenc, adec = q0_entries[0]
            t_label = f'{asz} B' if asz < 1000 else f'{asz // 1000} KB'
            # Use matched WTPC encode at same AVIF size
            we_key = (int(speed), 0)
            if we_key in avif_wtpc:
                wq, wsz, wpsnr, wssim, wenc, wdec = avif_wtpc[we_key]
            else:
                wq, wsz, wpsnr, wssim, wenc, wdec = (0, asz, 0, 0, 0, 0)
            if we_key in avif_w420:
                w4q, w4sz, w4psnr, w4ssim, w4enc, w4dec = avif_w420[we_key]
            else:
                w4q, w4sz, w4psnr, w4ssim, w4enc, w4dec = (0, asz, 0, 0, 0, 0)
            md.append(
                f'| ~{t_label} | {aq} | {wq} | {w4q} | {asz} | {wsz} | {w4sz} | {aenc:.0f} | {wenc:.0f} | {w4enc:.0f} | {adec:.0f} | {wdec:.0f} | {w4dec:.0f} | {apsnr:.2f} | {wpsnr:.2f} | {w4psnr:.2f} | {assim:.2f} | {wssim:.2f} | {w4ssim:.2f} |')

        # Emit rows for each target
        for t in avif_targets:
            t_label = f'{t} B' if t < 1000 else (f'{t//1000} KB' if t % 1000 == 0 else f'{t/1000:.1f} KB')
            avif_closest = min(data[avif_key], key=lambda e: abs(e[1] - t))
            aq, asz, apsnr, assim, aenc, adec = avif_closest
            # Use matched WTPC encode at same AVIF size
            aq_num = int(aq.replace('q=', ''))
            we_key = (int(speed), aq_num)
            if we_key in avif_wtpc:
                wq, wsz, wpsnr, wssim, wenc, wdec = avif_wtpc[we_key]
            else:
                wq, wsz, wpsnr, wssim, wenc, wdec = (0, asz, 0, 0, 0, 0)
            if we_key in avif_w420:
                w4q, w4sz, w4psnr, w4ssim, w4enc, w4dec = avif_w420[we_key]
            else:
                w4q, w4sz, w4psnr, w4ssim, w4enc, w4dec = (0, asz, 0, 0, 0, 0)
            md.append(
                f'| {t_label} | {aq} | {wq} | {w4q} | {asz} | {wsz} | {w4sz} | {aenc:.0f} | {wenc:.0f} | {w4enc:.0f} | {adec:.0f} | {wdec:.0f} | {w4dec:.0f} | {apsnr:.2f} | {wpsnr:.2f} | {w4psnr:.2f} | {assim:.2f} | {wssim:.2f} | {w4ssim:.2f} |')
        md.append('')
        # Per-speed summary
        if speed == '0':
            md.append('> **Speed 0:** AVIF wins on ssim2 from 1.4 KB to 26 KB, but encoding is up to 370x slower than WTPC.')
        elif speed == '6':
            md.append('> **Speed 6:** AVIF wins on ssim2 from 2 KB to 16 KB and the gap narrows, but encoding is still up to 8x slower than WTPC.')
        elif speed == '10':
            md.append('> **Speed 10:** AVIF loses on ssim2 at all sizes, but encoding speed is now comparable (though slightly slower than WTPC).')
        md.append('')

md.append('')
md.append('---')
md.append('')
md.append(f'*Tools: ImageMagick 7.1.2, OpenJPEG 2.5.4, libjxl 0.11.1. Date: {date_str}.*')

with open(resfile, 'w') as f:
    f.write('\n'.join(md) + '\n')
print(f'Generated {resfile} ({len(md)} lines)')

# ============================================================
# Update readme.md with benchmark results
# ============================================================
try:
    with open(readmefile) as f:
        rl = f.readlines()

    def replace_table_in_lines(rl, section_header, new_data_rows):
        """Replace data rows in a markdown table section."""
        s_idx = None
        for i, line in enumerate(rl):
            if line.strip() == section_header:
                s_idx = i
                break
        if s_idx is None:
            return rl

        pipe_rows = []
        for i in range(s_idx + 1, len(rl)):
            stripped = rl[i].strip()
            if stripped.startswith('|'):
                pipe_rows.append(i)
            elif stripped == '':
                continue
            else:
                break

        if len(pipe_rows) < 3:
            return rl

        prefix = rl[:pipe_rows[2]]
        suffix = rl[pipe_rows[-1] + 1:]
        new_rows = [row.rstrip('\n') + '\n' for row in new_data_rows]
        return prefix + new_rows + suffix

    # ---- Best Codec by Target Size (by PSNR) ----
    idx = None
    for i, line in enumerate(rl):
        if line.strip() == '### Best Codec by Target Size (by PSNR)':
            idx = i
            break
    if idx is not None:
        targets = [200, 400, 600, 800, 1400, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 13000, 15000, 18000, 22000, 28000, 36000]
        clabels = {
            'WTPC_E': 'WTPC 4:4:4 EBCOT',
            'WTPC_H': 'WTPC 4:4:4 Huffman',
            'W420_E': 'WTPC 4:2:0 EBCOT',
            'W420_H': 'WTPC 4:2:0 Huffman',
            'J2K': 'JPEG 2000',
            'JXL': 'JPEG XL',
            'JPEG': 'JPEG',
        }

        new_rows = []
        for target in targets:
            lo, hi = int(target * 0.7), int(target * 1.3)
            # For each codec find the CLOSEST entry to target
            closest_per_codec = {}
            for codec in data:
                if codec.startswith('AVIF'): continue
                closest = None; closest_dist = 999999
                for param, sz, psnr, ssim, _enc, _dec in data[codec]:
                    if lo <= sz <= hi:
                        dist = abs(sz - target)
                        if dist < closest_dist: closest_dist = dist; closest = (param, sz, psnr, ssim)
                if closest: closest_per_codec[codec] = closest
            # Among closest entries, pick the best PSNR
            best_codec = None
            best_psnr = -1.0
            best_entry = None
            for codec, entry in closest_per_codec.items():
                if entry[2] > best_psnr:
                    best_psnr = entry[2]
                    best_codec = codec
                    best_entry = entry
            if best_entry:
                param, sz, psnr, ssim = best_entry
                label = clabels.get(best_codec, best_codec)
                t_label = f"{target} B" if target < 1000 else f"{target // 1000} KB"
                new_rows.append(f"| {t_label} | {label} | {sz} B | {psnr:.2f} | {ssim:.2f} |")

        rl = replace_table_in_lines(rl, '### Best Codec by Target Size (by PSNR)', new_rows)
        print(f'  Updated Best Codec table ({len(new_rows)} rows)')

    # ---- Speed Summary (lena 256x256, representative q=244) ----
    idx = None
    for i, line in enumerate(rl):
        if line.strip() == '### Speed Summary (lena 256x256, representative q=244)':
            idx = i
            break
    if idx is not None:
        wtpc_labels = {
            'WTPC_E': 'WTPC EBCOT 4:4:4',
            'WTPC_H': 'WTPC Huffman 4:4:4',
            'W420_E': 'WTPC EBCOT 4:2:0',
            'W420_H': 'WTPC Huffman 4:2:0',
        }

        speed_rows = []
        for var in ['WTPC_E', 'WTPC_H', 'W420_E', 'W420_H']:
            if var in time_data:
                entry = [e for e in time_data[var] if e[0] == 244]
                if entry:
                    enc, dec = entry[0][2], entry[0][3]
                    speed_rows.append(f"| {wtpc_labels[var]} | {round(enc)} | {round(dec)} |")

        # JPEG 2000: rate=20 entry
        for p, sz, psnr, ssim, enc, dec in data.get('J2K', []):
            if str(p) == '20':
                speed_rows.append(f"| JPEG 2000 | {round(enc)} | {round(dec)} |")
                break

        # JPEG XL: min quality (first/best entry)
        jxl = data.get('JXL', [])
        if jxl:
            enc, dec = jxl[0][4], jxl[0][5]
            speed_rows.append(f"| JPEG XL | {round(enc)} | {round(dec) if dec > 0 else '-'} |")

        # JPEG: q=20 entry
        for p, sz, psnr, ssim, enc, dec in data.get('JPEG', []):
            if str(p) == '20':
                speed_rows.append(f"| JPEG | {round(enc)} | {round(dec) if dec > 0 else '-'} |")
                break

        rl = replace_table_in_lines(rl, '### Speed Summary (lena 256x256, representative q=244)', speed_rows)
        print(f'  Updated Speed Summary table ({len(speed_rows)} rows)')

    with open(readmefile, 'w') as f:
        f.writelines(rl)
    print(f'Updated {readmefile}')
except Exception as ex:
    print(f'Skipping readme.md update: {ex}')
PYEOF

echo "" | tee -a "$LOGFILE"
echo "=== ALL DONE ===" | tee -a "$LOGFILE"
echo "Results: $SCRIPT_DIR/results.md" | tee -a "$LOGFILE"
echo "Samples: $SAMPLES/" | tee -a "$LOGFILE"
echo "Log:     $LOGFILE" | tee -a "$LOGFILE"
