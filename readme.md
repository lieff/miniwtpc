# Small Wavelet Thumbnail & Preview Codec - WTPC

A simple, drop-in image codec in the style of stb_image (single header library).
It targets low sizes from 200 B to 36 KB at resolutions around 256x256, but supports any image dimensions up to 65536x65536.
The main target for thumbnails is 1400 B -- designed to fit within one MTU
packet, so the user sees *something* while the main preview downloads.

It has two modes: fast Huffman and slower EBCOT-lite (much simpler than
JPEG 2000 -- not even Tier-1, since that would need far more code).
A third mode, **wavelet hash** (`-m whash`), provides ultra-compact encoding (30-50 bytes for a 128-256 thumbnail) -
ideal for instant previews and placeholders (if 1400 B detailed preview is too much).
Despite its simplicity, WTPC outperforms JPEG 2000 and JPEG XL on this
small-image benchmark, likely because its quantization is tuned to sharpen
at low bitrates and the test dataset is relatively small (~3000 images).

Currently WIP, bitstream format is not yet stable and may change if further quality improvements are found.

**Confirmed to outperform** (by ssimulacra2, lena256.png 256x256, 200 B - 36 KB):
- JPEG (libjpeg)
- JPEG 2000 (OpenJPEG 2.5.4)
- HTJ2K (openjph 0.26.0, clear win in our range 200 B - 36 KB +1..+36 ssim2, HTJ2K wins only at >70 KB where WTPC hits int16 quality ceiling 96.12 ssim2, near lossless range; encode speed comparable 3-4ms vs WTPC 1-9ms)
- JPEG XL (libjxl 0.11.1)
- AVIF --speed 10 (avifenc 1.3.0; speeds 0/6 partially win but encoding is 8-370x slower)
- WebP (libwebp 1.5.0, clear win across all sizes +2..+15 ssim2, similar encode speed 1-9ms vs WebP 6-9ms)
- WebP 2 (libwebp2 0.0.1, partial: WTPC wins at <=2.6 KB and 36 KB, WebP2 wins at 2.9-21 KB but encoding is 4-10x slower)
- HEIF (libheif 1.21.2 / x265 4.1, partial: WTPC wins at <=3 KB and >=15 KB, HEIF wins at 3-15 KB but encoding is 3-5x slower and quality plateaus at ~91.8 ssim2)
- BPG 0.9.8 x265 (libbpg 0.9.8 / x265 4.1, partial: WTPC wins at <526 B (BPG minimum) and >=21 KB, BPG wins at 0.5-21 KB by +2..+12 ssim2 but encoding is 3-5x slower; quality plateaus at ~93 ssim2 vs WTPC 95)
- BPG 0.9.8 JCTVC (libbpg 0.9.8, partial: WTPC wins at <380 B (BPG minimum) and >=21 KB, BPG wins at 0.4-21 KB by +0.7..+12 ssim2 but encoding is 20-50x slower; quality plateaus at ~93 ssim2 vs WTPC 95)
- VC-2 SMPTE 2042-1 (reference encoder 0.1.0.2, Daub97 DD97 wavelets, clear win across all sizes +29..+50 ssim2)
- SPIHT (TiLib 1.0, Daub97 wavelet)
- GFWX 1.2 (Golomb-Rice entropy coder)
- SQZ (5/3 wavelet + WDR, no entropy coding)
- Ako 0.3.0 (CDF 9/7 + Kagari/ANS)
- NHW 0.3.3 (simple wavelet, speed-oriented, only 512x512)

## Large image support

Although WTPC is designed for thumbnails (target resolutions around 256x256),
it supports images up to 65536x65536 (practically tested up to ~27K with images loadable by stb_image).
For large images the `--block N` option splits encoding into independent
NxN blocks (32..2048), trading some compression for speed. The full-image
EBCOT mode achieves the best compression but becomes slow on large images (heavy EBCOT-BAC coder).

Comparison on sample big png (17 Mpix, 13 decomposition levels, 5 EXT bands) vs HTJ2K:

|            |  71 KB  |           | 112 KB  |           | 205 KB  |           |  543 KB  |           |
|------------|---------|-----------|---------|-----------|---------|-----------|----------|-----------|
|            |  SSIM2  |   enc time |  SSIM2  |   enc time |  SSIM2  |   enc time |  SSIM2   |  enc time |
| HTJ2K      |  14.28  |    61 ms   |  34.20  |    67 ms   |  55.79  |    97 ms   |  79.36   |    80 ms   |
| WTPC EBCOT full  | 32.36 |  867 ms |  49.27  |  1079 ms   |  68.13  |  1310 ms   |  84.90   |  2090 ms  |
| WTPC EBCOT --block 256 | 29.71 | 375 ms | 47.78 | 418 ms | 66.96 | 426 ms |  84.32   |   475 ms  |
| WTPC EBCOT --block 512 | 31.06 | 401 ms | 48.44 | 446 ms | 66.96 | 478 ms |  84.32   |   575 ms  |
| WTPC Huffman      | 20.69 |  303 ms |  40.17  |   294 ms   |  60.51  |   304 ms   |  81.24   |   209 ms  |
| WTPC Huffman ctx  | 21.87 |  306 ms |  41.06  |   300 ms   |  61.11  |   317 ms   |  81.24   |   208 ms  |

WTPC EBCOT wins at all sizes (+4..+16 ssim2 over HTJ2K) but the margin
narrows at higher quality levels. At large sizes HTJ2K is 10-25x faster
than WTPC full-image mode. Block mode bridges the speed gap (3-4x faster
than full at negligible quality cost ~0.4 ssim2). Huffman mode is the
fastest WTPC option, competitive with HTJ2K in speed while still slightly
better on quality (+5..+19 ssim2 at smaller sizes, +0.8 at 543KB).
Context-aware Huffman (-h 1) adds a small quality gain with negligible
speed cost.

## API and usage

### Integration (single-header library)

WTPC follows the stb_image pattern: the header `wtpc_image.h` contains both
the API declarations and the implementation, gated by a macro.

```c
// In ONE .c file, define the implementation macro before including:
#define WTPC_IMAGE_IMPLEMENTATION
#include "wtpc_image.h"

// Everywhere else, just include the header to get the API declarations:
#include "wtpc_image.h"
```

The header pulls in `<stdint.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`,
and optionally `<stdio.h>` (exclude with `WTPC_NO_STDIO`).  On x86-64 the
build script auto-enables AVX; add `-mavx` manually on other compilers.
Link with `-lm`.

### C API

```c
   === API ===

   typedef enum {
       WTPC_ENC_AUTO    = 0,
       WTPC_ENC_HUFFMAN = 1,
       WTPC_ENC_EBCOT   = 2,
       WTPC_ENC_WHASH   = 3
   } wtpc_enc_mode;

   typedef struct {
       int encoded_bytes;   - output number of bytes
       int result_q;        - resulting quantization factor if target_bytes provided, or same as 'quality' if target_bytes=0
       int search_steps;    - number of iterations to search target bytes quantization 
       int ebcot;           - 1 = ebcot or 0 = huffman mode for best pick if auto encode_mode used
       int huffman_y_size;  - in bits if not picked static table
       int huffman_u_size;
       int huffman_v_size;
       int huffman_y_table; - 0..NUM_DEF_TABLES-1 - static, NUM_DEF_TABLES - custom written in bitstream
       int huffman_u_table;
       int huffman_v_table;
   } wtpc_enc_info;

   unsigned char *wtpc_encode_mem(const unsigned char *rgb, wtpc_enc_info *info,
       int w, int h, int target_bytes, int quality, int chroma_420,
       int encode_mode, int huf_extra_ctx, int has_alpha, int stride, int block_size);
     Encode an RGB/RGBA image in memory. Returns malloc'd WTPC bitstream,
     or NULL on error. Caller must free().
       rgb           : input pixels, h rows of stride bytes each.
                        Each row has w pixels, 3 bytes/pixel (RGB) or 4 (RGBA).
       info          : output struct, filled with encoding details (may be NULL).
       w, h          : image dimensions (>= 1).
       target_bytes  : desired output size in bytes. 0 = use 'quality' instead.
                       When > 0, the encoder does a binary search over the
                       quality range [1..MAX_QUALITY] to hit the target.
       quality       : quantization level 1..MAX_QUALITY (1024). Lower = better
                       quality / larger file. Used only when target_bytes == 0.
       chroma_420    : 0 = 4:4:4 (full chroma), 1 = 4:2:0 (half chroma).
                       4:2:0 saves ~15-30% bytes with minor visual loss.
       encode_mode   : WTPC_ENC_AUTO (auto-pick ebcot/huffman),
                       WTPC_ENC_HUFFMAN, WTPC_ENC_EBCOT.
       huf_extra_ctx : 0 = single Huffman table (faster),
                       1 = two context-switched tables (slightly better).
       has_alpha     : 0 = RGB (3 channels), 1 = RGBA (4 channels).
       stride        : bytes per row (0 = tightly packed = w * pixel_bytes).
                        Allows BMP-like padded data without repacking.
       block_size    : 0 = normal encode, or precinct size for EBCOT block mode
                       (32, 64, 128, 256, 512, 1024, 2048). Full-image wavelet,
                       entropy coding split into NxN blocks for speed. 3-4x faster
                       on large images; only valid with EBCOT (not Huffman).

   unsigned char *wtpc_decode_mem(const unsigned char *data, int data_len,
       int *w, int *h, int *out_quality, int *out_comp);
     Decode a WTPC bitstream from memory. Returns malloc'd pixel buffer
     (w*h*3 for RGB, w*h*4 for RGBA). Caller must free().
       data          : input WTPC bitstream bytes.
       data_len      : number of bytes in 'data'.
       w, h          : output image dimensions.
       out_quality   : quality level used for encoding (may be NULL).
       out_comp      : number of color components: 3 = RGB, 4 = RGBA (may be NULL).

   int wtpc_encode_file(const char *out_path, const unsigned char *rgb,
       wtpc_enc_info *info, int w, int h, int target_bytes, int quality,
       int chroma_420, int encode_mode, int huf_extra_ctx, int has_alpha, int stride, int block_size);
     Same as wtpc_encode_mem but writes directly to a file.
     Returns 0 on success, -1 on error.

   unsigned char *wtpc_decode_file(const char *in_path,
       int *w, int *h, int *out_quality, int *out_comp);
     Same as wtpc_decode_mem but reads from a file.

   === Wavelet Hash API (ultra-compact, ThumbHash-like) ===

   uint8_t *wtpc_hash_encode_mem(const uint8_t *rgb, int w, int h, int *out_len);
     Encode an RGB image (w,h <= 256) to a compact wavelet hash.
     Uses CDF 9/7 wavelet + YUV color space, auto-budget from image size.
     Returns malloc'd hash buffer, or NULL on error. Caller must free().
       rgb     : input pixels, tightly packed RGB (w*h*3 bytes).
       w, h    : image dimensions (1..256).
       out_len : filled with hash size in bytes.

   uint8_t *wtpc_hash_decode_mem(const uint8_t *hash, int hash_len, int *w, int *h);
     Decode a wavelet hash back to w x h RGB image.
     Returns malloc'd pixel buffer (w*h*3 bytes), or NULL on error. Caller must free().
       hash     : input hash bytes (from wtpc_hash_encode_mem).
       hash_len : number of bytes in hash.
       w, h     : filled with decoded image dimensions.

   === Build-time options ===
     #define WTPC_NO_STDIO        : exclude file I/O functions.
     #define DEBUG_WAVELET        : dump wavelet coefficient images (needs stb).
     #define STANDARD_CDF97       : enable standard CDF 9/7 K-scaling.
     #define BAC_USE_TABLE        : use 64 KB reciprocal lookup table for
                                    BAC division (~+1-3% speed, 64 KB memory).
                                    Default: 64-bit integer division.
     #define WTPC_TUNE_PARAMS     : mutable quantization tables for grid-search tuning.
     #define WTPC_TUNE_CTX        : tune ebcot contexts
     #define WTPC_NO_SIMD         : do not use sse/avx/neon intrinsics.
     #define WTPC_RC_ONLY_LESS_THAN_TARGET : rate control never overshoots
                                    target_bytes (picks the largest size <= target
                                    instead of the closest). Implied by
                                    WTPC_TUNE_PARAMS.
```

### CLI tool flags

The standalone `wtpc` binary (built via `build.sh` or `gcc -O3 wtpc.c -o wtpc -lm -lpng16`) uses these flags:

| Flag | Description |
|------|-------------|
| `-e in.png` | Encode mode (requires `-o out.wtp`). With `-m whash`: encode wavelet hash -> `.whash` |
| `-d in.wtp` | Decode mode (requires `-o out.png`). With `.whash` input: auto-detect hash decode |
| `-t in.png` | Self-test: encode + decode + compare PSNR |
| `-q N` | Quality 1..1024 (lower = better/larger) |
| `-b N` | Target file size in bytes (auto-finds q) |
| `-c` | Use 4:2:0 chroma subsampling |
| `-m best\|ebcot\|huffman\|whash` | Encoding mode (default: ebcot). `whash` = wavelet hash, ultra-compact (~30-50 bytes for thumbnails) |
| `-h 1` | Context-aware Huffman tables (slower, slightly better) |
| `--block N` | EBCOT mode only: split bit planes into NxN blocks for fast large-image encoding (32/64/128/256/512/1024/2048) |
| `-o file` | Output file path |
| `-G dir` | Generate Huffman tables from images in directory |
| `-P dir` | Tune EBCOT contexts from images (needs `WTPC_TUNE_CTX`) |
| `-T dir` | Tune quantization parameters (needs `WTPC_TUNE_PARAMS`) |
| `-R dir` | Train DC priors (needs `WTPC_TUNE_PARAMS`) |
| `-S N` | Start tuning from parameter set N |
| `-420` | Tune 4:2:0 mode (with `-T` / `-R`) |
| `-v` | Verbose tuning output |

### Tuning and retraining

You can retrain quantization parameters, DC priors, Huffman tables, and
EBCOT contexts on your own dataset:

1. Build with `WTPC_TUNE_PARAMS` (and optionally `WTPC_TUNE_CTX` for -P)
2. Run `./wtpc -T images/` to tune quantization tables
3. Run `./wtpc -R images/` to train DC priors
4. Run `./wtpc -G images/` to generate Huffman tables
5. Run `./wtpc -P images/` to tune EBCOT contexts (requires WTPC_TUNE_CTX instead of WTPC_TUNE_PARAMS)
6. Paste the printed tables back into `wtpc_image.h`

Use `-420` to tune the 4:2:0 variants, `-S N` to continue from a specific
parameter set, and `-v` for verbose progress output.

**Note:** tuning changes the bitstream format, making it incompatible with
the release version.

## Benchmark: WTPC vs JPEG vs JPEG 2000 vs JPEG XL

**Test image:** `lena256.png` (256x256, 24-bit RGB)  
**Target range:** 200 B -- 36 KB  
**Metrics:** PSNR (dB, higher is better), ssimulacra2 (higher is better)  
**Full results:** [results.md](results.md)

### Best Codec by Target Size (by PSNR)

| Target | Best Codec         | Size   | PSNR   | ssimulacra2 |
|--------|--------------------|--------|--------|-------------|
| 200 B | WTPC 4:2:0 EBCOT | 201 B | 19.91 | -59.43 |
| 400 B | WTPC 4:4:4 EBCOT | 405 B | 22.14 | -38.02 |
| 600 B | WTPC 4:2:0 EBCOT | 604 B | 23.12 | -21.20 |
| 800 B | WTPC 4:2:0 EBCOT | 801 B | 24.13 | -5.90 |
| 1 KB | WTPC 4:4:4 EBCOT | 1401 B | 26.02 | 21.74 |
| 2 KB | WTPC 4:4:4 EBCOT | 2009 B | 27.18 | 37.19 |
| 3 KB | WTPC 4:4:4 EBCOT | 3004 B | 28.58 | 50.26 |
| 4 KB | WTPC 4:4:4 EBCOT | 3992 B | 29.71 | 59.29 |
| 5 KB | WTPC 4:4:4 EBCOT | 5015 B | 30.70 | 65.56 |
| 6 KB | WTPC 4:4:4 EBCOT | 6013 B | 31.62 | 70.32 |
| 8 KB | WTPC 4:4:4 EBCOT | 8010 B | 33.14 | 76.08 |
| 10 KB | WTPC 4:4:4 EBCOT | 10021 B | 34.46 | 80.29 |
| 13 KB | WTPC 4:4:4 EBCOT | 12988 B | 35.96 | 84.39 |
| 15 KB | WTPC 4:4:4 EBCOT | 14992 B | 36.76 | 86.53 |
| 18 KB | WTPC 4:4:4 EBCOT | 18003 B | 37.82 | 88.84 |
| 22 KB | WTPC 4:4:4 EBCOT | 22032 B | 39.09 | 90.67 |
| 28 KB | WTPC 4:4:4 EBCOT | 27993 B | 40.70 | 92.54 |
| 36 KB | WTPC 4:4:4 EBCOT | 35964 B | 42.55 | 93.93 |

### Speed Summary (lena 256x256, representative q=244)

| Codec               | Encode (ms) | Decode (ms) |
|---------------------|-------------|-------------|
| WTPC EBCOT 4:4:4 | 7 | 7 |
| WTPC Huffman 4:4:4 | 1 | 1 |
| WTPC EBCOT 4:2:0 | 5 | 5 |
| WTPC Huffman 4:2:0 | 1 | 1 |
| JPEG 2000 | 16 | 5 |
| JPEG XL | 103 | 3 |
| JPEG | 4 | 3 |

See [results.md](results.md) for the complete per-size breakdown, speed
measurements across all quality levels, mermaid charts, and raw data.

### Visual Comparison (lena 256x256)

Click any image to view full size.

**1.4 KB** -- thumbnail target (worst quality)

| WTPC EBCOT | WTPC Huffman | JPEG 2000 | JPEG XL | JPEG |
|:----------:|:------------:|:---------:|:-------:|:----:|
| ![](samples/WTPC_E_worst_1.4kb.png) | ![](samples/WTPC_H_worst_1.4kb.png) | ![](samples/JP2K_worst_1.4kb.png) | ![](samples/JXL_worst_1.4kb.png) | ![](samples/JPEG_worst_1.4kb.jpg) |

**6 KB** -- preview (mid quality)

| WTPC EBCOT | WTPC Huffman | JPEG 2000 | JPEG XL | JPEG |
|:----------:|:------------:|:---------:|:-------:|:----:|
| ![](samples/WTPC_E_mid_6kb.png) | ![](samples/WTPC_H_mid_6kb.png) | ![](samples/JP2K_mid_6kb.png) | ![](samples/JXL_mid_6kb.png) | ![](samples/JPEG_mid_6kb.jpg) |

**13 KB** -- good quality

| WTPC EBCOT | WTPC Huffman | JPEG 2000 | JPEG XL | JPEG |
|:----------:|:------------:|:---------:|:-------:|:----:|
| ![](samples/WTPC_E_good_13kb.png) | ![](samples/WTPC_H_good_13kb.png) | ![](samples/JP2K_good_13kb.png) | ![](samples/JXL_good_13kb.png) | ![](samples/JPEG_good_13kb.jpg) |

**36 KB** -- best quality

| WTPC EBCOT | WTPC Huffman | JPEG 2000 | JPEG XL | JPEG |
|:----------:|:------------:|:---------:|:-------:|:----:|
| ![](samples/WTPC_E_best_36kb.png) | ![](samples/WTPC_H_best_36kb.png) | ![](samples/JP2K_best_36kb.png) | ![](samples/JXL_best_36kb.png) | ![](samples/JPEG_best_36kb.jpg) |

**200 B -- 1.2 KB** -- ultra-low bitrates (JPEG XL cannot reach this range)

| Size | WTPC EBCOT | JPEG 2000 | JPEG |
|:----:|:----------:|:---------:|:----:|
| 200 B | ![](samples/WTPC_200b.png) | ![](samples/JP2K_200b.png) | - |
| 400 B | ![](samples/WTPC_400b.png) | ![](samples/JP2K_400b.png) | - |
| 600 B | ![](samples/WTPC_600b.png) | ![](samples/JP2K_600b.png) | - |
| 800 B | ![](samples/WTPC_800b.png) | ![](samples/JP2K_800b.png) | - |
| 1000 B | ![](samples/WTPC_1000b.png) | ![](samples/JP2K_1000b.png) | ![](samples/JPEG_1000b.jpg) |
| 1200 B | ![](samples/WTPC_1200b.png) | ![](samples/JP2K_1200b.png) | ![](samples/JPEG_1200b.jpg) |

**AVIF --speed 6 vs WTPC (best ssim2)** -- mid-speed AVIF vs best WTPC at equal file sizes

| Size | AVIF (--speed 6) | WTPC (best by ssim2) |
|:----:|:----------------:|:--------------------:|
| ~726 B | ![](samples/AVIF_S6_726b.png) | ![](samples/WTPC_vs_AVIF_726b.png) |
| 1 KB | ![](samples/AVIF_S6_1kb.png) | ![](samples/WTPC_vs_AVIF_1kb.png) |
| 1.4 KB | ![](samples/AVIF_S6_1.4kb.png) | ![](samples/WTPC_vs_AVIF_1.4kb.png) |
| 2 KB | ![](samples/AVIF_S6_2kb.png) | ![](samples/WTPC_vs_AVIF_2kb.png) |
| 4 KB | ![](samples/AVIF_S6_4kb.png) | ![](samples/WTPC_vs_AVIF_4kb.png) |
| 16 KB | ![](samples/AVIF_S6_16kb.png) | ![](samples/WTPC_vs_AVIF_16kb.png) |
| 36 KB | ![](samples/AVIF_S6_36kb.png) | ![](samples/WTPC_vs_AVIF_36kb.png) |

**Whash vs ThumbHash** -- wavelet hash vs DCT hash on lena 256x256

| lena 256x256 | WTPC Whash (48 B) | ThumbHash (24 B) |
|:------------:|:-----------------:|:----------------:|
| ![](lena256.png) | ![](samples/lena256.whash.png) | ![](samples/lena128.thumbhash.png) |

Whash uses **48 bytes** (vs ThumbHash's 24 bytes, downscaled to 128x128) but preserves significantly
more detail and support up to 256x256 resolution (vs ThumbHash's 128x128).

## Interesting Links

 * https://github.com/nothings/stb
 * https://github.com/kalcutter/gfwx
 * https://github.com/Themaister/pyrowave
 * https://github.com/MarcioPais/SQZ
 * https://github.com/josejuansanchez/bgp-image-format
 * https://bellard.org/bpg/
 * https://github.com/LMP88959/Digital-Subband-Video-2
 * https://github.com/curioustorvald/TAV-video-codec
 * https://github.com/datocms/fast_thumbhash
 * https://github.com/gopro/cineform-sdk
 * https://github.com/emericg/libcineform
 * https://github.com/bbc/vc2-reference
 * https://github.com/rcanut/nhwcodec
 * https://github.com/baAlex/Ako
 * https://github.com/Special-graphic-formats/tilib
 * https://themaister.net/blog/2025/06/16/i-designed-my-own-ridiculously-fast-game-streaming-video-codec-pyrowave/

## Image Datasets

 * https://github.com/imazen/codec-corpus
 * https://github.com/castano/image-datasets
 * https://jpegai.github.io/test_images/
 * https://github.com/EliSchwartz/imagenet-sample-images
 * https://cloudinary.com/labs/cid22
 * https://www.imageprocessingplace.com/root_files_V3/image_databases.htm
 * https://samplelib.com/sample-png.html
 * https://www.stickpng.com/
