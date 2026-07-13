/*
   WTPC - Small Wavelet Thumbnail & Preview Codec - https://github.com/lieff/miniwtpc
   No warranty implied; use at your own risk.
   LICENSE: Dual-licensed under the Public Domain and the MIT License.

   === API ===

   typedef struct {
       int encoded_bytes;   - output number of bytes
       int result_q;        - resulting quantization factor if target_bytes provided, or same as 'quality' if target_bytes=0
       int search_steps;    - number of iterations to search target bytes quantization 
       int ebcot;           - 1 = ebcot or 0 = huffman mode for best pick if auto huffman_mode used
       int huffman_y_size;  - in bits if not picked static table
       int huffman_u_size;
       int huffman_v_size;
       int huffman_y_table; - 0..NUM_DEF_TABLES-1 - static, NUM_DEF_TABLES - custom written in bitstream
       int huffman_u_table;
       int huffman_v_table;
   } wtpc_enc_info;

   unsigned char *wtpc_encode_mem(const unsigned char *rgb, wtpc_enc_info *info,
       int w, int h, int target_bytes, int quality, int chroma_420,
       int huffman_mode, int huf_extra_ctx, int has_alpha);
     Encode an RGB/RGBA image in memory. Returns malloc'd WTPC bitstream,
     or NULL on error. Caller must free().
       rgb           : input pixels, w*h*3 bytes (RGB) or w*h*4 (RGBA).
       info          : output struct, filled with encoding details (may be NULL).
       w, h          : image dimensions (>= 1).
       target_bytes  : desired output size in bytes. 0 = use 'quality' instead.
                       When > 0, the encoder does a binary search over the
                       quality range [1..MAX_QUALITY] to hit the target.
       quality       : quantization level 1..MAX_QUALITY (1024). Lower = better
                       quality / larger file. Used only when target_bytes == 0.
       chroma_420    : 0 = 4:4:4 (full chroma), 1 = 4:2:0 (half chroma).
                       4:2:0 saves ~15-30% bytes with minor visual loss.
       huffman_mode  : 0 = auto-pick smaller of ebcot/huffman,
                       1 = huffman, 2 = ebcot.
       huf_extra_ctx : 0 = single Huffman table (faster),
                       1 = two context-switched tables (slightly better).
       has_alpha     : 0 = RGB (3 channels), 1 = RGBA (4 channels).

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
       int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha);
     Same as wtpc_encode_mem but writes directly to a file.
     Returns 0 on success, -1 on error.

   unsigned char *wtpc_decode_file(const char *in_path,
       int *w, int *h, int *out_quality, int *out_comp);
     Same as wtpc_decode_mem but reads from a file.

   === Build-time options ===
     #define WTPC_NO_STDIO        : exclude file I/O functions.
     #define DEBUG_WAVELET        : dump wavelet coefficient images (needs stb).
     #define STANDARD_CDF97       : enable standard CDF 9/7 K-scaling.
     #define WTPC_TUNE_PARAMS     : mutable quantization tables for grid-search tuning.
     #define WTPC_RC_ONLY_LESS_THAN_TARGET : rate control never overshoots
                                    target_bytes (picks the largest size <= target
                                    instead of the closest). Implied by
                                    WTPC_TUNE_PARAMS.
*/
#ifndef WTPC_INCLUDE_WTPC_IMAGE_H
#define WTPC_INCLUDE_WTPC_IMAGE_H

/*#define WTPC_NO_STDIO*/     /* exclude stdio versions */
/*#define DEBUG_WAVELET*/     /* dump wavelet coefficients, requires stb image */
/*#define STANDARD_CDF97*/    /* enable standard CDF 9/7 K-scaling */
/*#define WTPC_RC_ONLY_LESS_THAN_TARGET*/  /* rate control: never overshoot target_bytes */
/*#define WTPC_TUNE_PARAMS*/  /* quantization params tuning mode */

#ifndef WTPC_NO_STDIO
#include <stdio.h>
#endif  /*  WTPC_NO_STDIO */
#include <stdint.h>

#define MAX_QUALITY 1024

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int encoded_bytes;
    int result_q;        /* if target_bytes provided */
    int search_steps;    /* number of iterations to search target bytes quantization */
    int ebcot;           /* 1 = ebcot or 0 = huffman mode for best pick */
    int huffman_y_size;  /* in bits if not picked static table */
    int huffman_u_size;
    int huffman_v_size;
    int huffman_y_table; /* 0..NUM_DEF_TABLES-1 - static, NUM_DEF_TABLES - custom written in bitstream */
    int huffman_u_table;
    int huffman_v_table;
} wtpc_enc_info;

typedef struct {
    uint8_t *data;
    int byte_pos;
    int buf_size;
    uint32_t cache;
    int bits_in_cache;
} Bitstream;


unsigned char *wtpc_encode_mem(const unsigned char *rgb, wtpc_enc_info *info, int w, int h, int target_bytes, int quality, int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha);
unsigned char *wtpc_decode_mem(const unsigned char *data, int data_len, int *w, int *h, int *out_quality, int *out_comp);

#ifndef WTPC_NO_STDIO
int wtpc_encode_file(const char *out_path, const unsigned char *rgb, wtpc_enc_info *info, int w, int h, int target_bytes, int quality, int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha);
unsigned char *wtpc_decode_file(const char *in_path, int *w, int *h, int *out_quality, int *out_comp);
#endif  /* WTPC_NO_STDIO */

#ifdef __cplusplus
}
#endif

#endif  /* WTPC_INCLUDE_WTPC_H */

#ifdef WTPC_IMAGE_IMPLEMENTATION

/* Rate control "never overshoot" mode: pick the largest encoding that still fits
   under target_bytes rather than the one closest to it. Tuning always needs this
   (overshoot would cheat the metrics), so WTPC_TUNE_PARAMS implies it. */
#if defined(WTPC_TUNE_PARAMS) && !defined(WTPC_RC_ONLY_LESS_THAN_TARGET)
#define WTPC_RC_ONLY_LESS_THAN_TARGET
#endif

#ifdef _WIN32
#define inline __inline
#include <intrin.h>
#pragma intrinsic(_BitScanReverse)
static __inline int msvc_clz32(unsigned int x) {
    unsigned long idx; _BitScanReverse(&idx, x); return 31 - (int)idx;
}
#define __builtin_clz(x) msvc_clz32((unsigned int)(x))
#endif

static void bitstream_init(Bitstream *bs, const uint8_t *buffer, int buf_size) {
    bs->data = (uint8_t*)buffer;
    bs->byte_pos = 0;
    bs->buf_size = buf_size;
    bs->cache = 0;
    bs->bits_in_cache = 0;
}

static void bitstream_flush(Bitstream *bs) {
    if (bs->bits_in_cache > 0) {
        if (bs->byte_pos >= bs->buf_size) { bs->bits_in_cache = 0; return; }
        bs->data[bs->byte_pos++] = (uint8_t)(bs->cache << (8 - bs->bits_in_cache));
        bs->cache = 0;
        bs->bits_in_cache = 0;
    }
}

static int bitstream_bits(Bitstream *bs) {
    return bs->byte_pos*8+bs->bits_in_cache;
}

static int bitstream_bytes(Bitstream *bs) {
    return bs->byte_pos;
}

static void put_bits(Bitstream *bs, uint32_t value, int num_bits) {
    value &= ((1ULL << num_bits) - 1);
    bs->cache = (bs->cache << num_bits) | value;
    bs->bits_in_cache += num_bits;
    while (bs->bits_in_cache >= 8) {
        bs->bits_in_cache -= 8;
        if (bs->byte_pos >= bs->buf_size) { bs->byte_pos++; return; }  /* overflow guard */
        bs->data[bs->byte_pos++] = (uint8_t)(bs->cache >> bs->bits_in_cache);
    }
}

static uint32_t get_bits(Bitstream *bs, int num_bits) {
    while (bs->bits_in_cache <= 24) {
        uint32_t next_byte = (bs->byte_pos < bs->buf_size) ? bs->data[bs->byte_pos++] : 0;
        bs->cache = (bs->cache << 8) | next_byte;
        bs->bits_in_cache += 8;
    }
    bs->bits_in_cache -= num_bits;
    uint32_t value = (bs->cache >> bs->bits_in_cache) & (((1ULL << num_bits) - 1));
    return value;
}

/* --- Big-endian uint16 (endian-agnostic file format) --- */
static inline void put_u16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v);
}
static inline uint16_t get_u16(const uint8_t *src) {
    return ((uint16_t)src[0] << 8) | src[1];
}

/* --- Exp-Golomb coding (unsigned, order k=0) --- */
static void put_eg(Bitstream *bs, uint32_t v) {
    v++;  /* EG(k=0): code = value + 1, so 0->"1", 1->"010", 2->"011", ... */
    if (v < 2) { put_bits(bs, 1, 1); return; }
    int m = 31 - __builtin_clz(v);  /* floor(log2(v)), v>=2 -> clz safe */
    for (int i = 0; i < m; i++) put_bits(bs, 0, 1);
    put_bits(bs, 1, 1);
    put_bits(bs, v - (1u << m), m);
}
static uint32_t get_eg(Bitstream *bs) {
    int m = 0; while (m < 32 && get_bits(bs, 1) == 0) m++;
    if (m >= 31) return 0;  /* overflow guard */
    uint32_t v = 1u << m;
    if (m > 0) v |= get_bits(bs, m);
    return v - 1;
}

/* BT.601 YUV color conversion. */
/* Y, U, V all centered around 0 (Y in -128..127, U/V in -128..127) so that */
/* wavelet LL does not accumulate a large DC bias (halves worst-case LL coeff, */
/* improving int16 headroom on large images and giving symmetric dead-zone). */
static inline void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, float *y, float *u, float *v) {
    *y =  0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
    *u = -0.1687f * r - 0.3313f * g + 0.5f * b;
    *v =  0.5f * r - 0.4187f * g - 0.0813f * b;
}

static inline void yuv_to_rgb(float y, float u, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    y += 128.0f;
    float r_f = y + 1.402f * v;
    float g_f = y - 0.34414f * u - 0.71414f * v;
    float b_f = y + 1.772f * u;
    *r = (uint8_t)(r_f < 0 ? 0 : (r_f > 255 ? 255 : roundf(r_f)));
    *g = (uint8_t)(g_f < 0 ? 0 : (g_f > 255 ? 255 : roundf(g_f)));
    *b = (uint8_t)(b_f < 0 ? 0 : (b_f > 255 ? 255 : roundf(b_f)));
}

/* --- 4:2:0 chroma down/up sampling (high quality) --- */
static void chroma_down_420(float *src, int w, int h, float *dst) {
    int hw = (w+1)/2, hh = (h+1)/2;
    for (int y = 0; y < hh; y++) {
        for (int x = 0; x < hw; x++) {
            float sum = 0; int cnt = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int sx = x*2+dx, sy = y*2+dy;
                    if (sx < w && sy < h) { sum += src[sy*w + sx]; cnt++; }
                }
            }
            dst[y*hw + x] = sum / cnt;
        }
    }
}
static void chroma_up_420(float *src, int sw, int sh, float *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        /* Downsampled sample i sits at full-res center (i+0.5)*dw/sw.            */
        /* For target full-res y the corresponding src coordinate is y*sh/dh-0.5. */
        /* Clamp to [0, sh-1] so the fractional part stays in [0, 1).             */
        float fy = ((float)y + 0.5f) * sh / dh - 0.5f;
        if (fy < 0.0f) fy = 0.0f;
        int iy = (int)fy; if (iy >= sh-1) iy = (sh > 1) ? sh-2 : 0;
        int iy1 = (iy + 1 < sh) ? iy + 1 : iy;  /* clamp neighbour (sh==1) */
        float wy = fy - (float)iy;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * sw / dw - 0.5f;
            if (fx < 0.0f) fx = 0.0f;
            int ix = (int)fx; if (ix >= sw-1) ix = (sw > 1) ? sw-2 : 0;
            int ix1 = (ix + 1 < sw) ? ix + 1 : ix;  /* clamp neighbour (sw==1) */
            float wx = fx - (float)ix;
            float v00 = src[iy*sw + ix], v10 = src[iy*sw + ix1];
            float v01 = src[iy1*sw + ix], v11 = src[iy1*sw + ix1];
            dst[y*dw + x] = v00*(1-wx)*(1-wy) + v10*wx*(1-wy) + v01*(1-wx)*wy + v11*wx*wy;
        }
    }
}

/* --- CDF 9/7 WAVELET (2D LIFTING, JPEG2000 LOSSY) --- */
/* Lifting constants (double precision). */
/* Symmetric boundary extension. */
/* Standard K-scaling (CDF97_K=1.1496) omitted - compensated by quantization. Set STANDARD_CDF97 to enable. */
#define CDF97_A  -1.586134342059924
#define CDF97_B  -0.052980118572961
#define CDF97_G   0.882911075530934
#define CDF97_D   0.443506852043971
#ifdef STANDARD_CDF97
#define CDF97_K      1.14960439886025f
#define CDF97_INVK   (1.0f / CDF97_K)
#else
#define CDF97_K      1.0f
#define CDF97_INVK   1.0f
#endif

static void cdf97_forward_2d(float *data, int width, int height) {
    int max_dim = width > height ? width : height;
    float *temp_l = (float*)malloc(max_dim * sizeof(float));
    float *temp_h = (float*)malloc(max_dim * sizeof(float));

    int w = width, h = height;
    while (w > 1 || h > 1) {
        int low_w = (w + 1) / 2, high_w = w / 2;  /* ceil, floor */
        int low_h = (h + 1) / 2, high_h = h / 2;

        /* --- Horizontal lifting (first w columns, first h rows) --- */
        if (w > 1) {
            for (int row = 0; row < h; ++row) {
                float *r = data + row * width;
                for (int j = 0; j < high_w; ++j) {
                    temp_l[j] = r[2*j];
                    temp_h[j] = r[2*j + 1];
                }
                if (w & 1) temp_l[low_w - 1] = r[w - 1];  /* odd width: extra lowpass */
                for (int j = 0; j < high_w; ++j) {
                    int jn = (j + 1 < low_w) ? j + 1 : low_w - 1;
                    temp_h[j] += (float)(CDF97_A * (temp_l[j] + temp_l[jn]));
                }
                for (int j = 0; j < low_w; ++j) {
                    int jp = (j > 0) ? j - 1 : 0;
                    int jj = (j < high_w) ? j : high_w - 1;
                    temp_l[j] += (float)(CDF97_B * (temp_h[jp] + temp_h[jj]));
                }
                for (int j = 0; j < high_w; ++j) {
                    int jn = (j + 1 < low_w) ? j + 1 : low_w - 1;
                    temp_h[j] += (float)(CDF97_G * (temp_l[j] + temp_l[jn]));
                }
                for (int j = 0; j < low_w; ++j) {
                    int jp = (j > 0) ? j - 1 : 0;
                    int jj = (j < high_w) ? j : high_w - 1;
                    temp_l[j] += (float)(CDF97_D * (temp_h[jp] + temp_h[jj]));
                }
                /* Write back: lowpass first, then highpass. K-scaling optional: low *= K, high /= K */
                for (int j = 0; j < low_w; ++j)  r[j] = temp_l[j] * CDF97_K;
                for (int j = 0; j < high_w; ++j) r[low_w + j] = temp_h[j] * CDF97_INVK;
            }
        }
        /* --- Vertical lifting (first w columns, first h rows) --- */
        if (h > 1) {
            for (int col = 0; col < w; ++col) {
                for (int i = 0; i < high_h; ++i) {
                    temp_l[i] = data[(2*i)     * width + col];
                    temp_h[i] = data[(2*i + 1) * width + col];
                }
                if (h & 1) temp_l[low_h - 1] = data[(h - 1) * width + col];
                for (int i = 0; i < high_h; ++i) {
                    int in = (i + 1 < low_h) ? i + 1 : low_h - 1;
                    temp_h[i] += (float)(CDF97_A * (temp_l[i] + temp_l[in]));
                }
                for (int i = 0; i < low_h; ++i) {
                    int ip = (i > 0) ? i - 1 : 0;
                    int ii = (i < high_h) ? i : high_h - 1;
                    temp_l[i] += (float)(CDF97_B * (temp_h[ip] + temp_h[ii]));
                }
                for (int i = 0; i < high_h; ++i) {
                    int in = (i + 1 < low_h) ? i + 1 : low_h - 1;
                    temp_h[i] += (float)(CDF97_G * (temp_l[i] + temp_l[in]));
                }
                for (int i = 0; i < low_h; ++i) {
                    int ip = (i > 0) ? i - 1 : 0;
                    int ii = (i < high_h) ? i : high_h - 1;
                    temp_l[i] += (float)(CDF97_D * (temp_h[ip] + temp_h[ii]));
                }
                for (int i = 0; i < low_h; ++i)  data[i        * width + col] = temp_l[i] * CDF97_K;
                for (int i = 0; i < high_h; ++i) data[(low_h + i) * width + col] = temp_h[i] * CDF97_INVK;
            }
        }
        w = low_w; h = low_h;
    }
    free(temp_l); free(temp_h);
}

static void cdf97_inverse_2d(float *data, int width, int height) {
    int max_dim = width > height ? width : height;
    float *temp_l = (float*)malloc(max_dim * sizeof(float));
    float *temp_h = (float*)malloc(max_dim * sizeof(float));

    /* Precompute LL sizes at each level (forward order, excluding level 0) */
    int ws[32], hs[32];
    int levels = 0;
    int w = width, h = height;
    while (w > 1 || h > 1) {
        ws[levels] = w; hs[levels] = h;
        w = (w + 1) / 2; h = (h + 1) / 2;
        levels++;
    }
    /* Inverse: from coarsest LL (w,h) up to full (width,height) */
    for (int lev = levels - 1; lev >= 0; lev--) {
        int prev_w = ws[lev], prev_h = hs[lev];  /* target size at this level */
        int low_w = w, low_h = h;  /* current LL size */
        int high_w = prev_w - low_w, high_h = prev_h - low_h;
#ifdef STANDARD_CDF97
        /* Unscale subbands (undo forward K-scaling). */
        /* LL gets xK for each dimension that had lifting (/Kv to undo). */
        /* HH gets /K for each dimension (xKv to undo). */
        float Kv = 1.0f;
        if (high_w > 0) Kv *= CDF97_K;
        if (high_h > 0) Kv *= CDF97_K;
        for (int y = 0; y < low_h; y++)
            for (int x = 0; x < low_w; x++)
                data[y * width + x] /= Kv;
        int hh_x0 = (high_w > 0) ? low_w : 0;
        int hh_y0 = (high_h > 0) ? low_h : 0;
        for (int y = hh_y0; y < prev_h; y++)
            for (int x = hh_x0; x < prev_w; x++)
                data[y * width + x] *= Kv;
#endif
        /* --- Inverse vertical lifting (prev_w cols x prev_h rows) --- */
        if (high_h > 0)
        for (int col = 0; col < prev_w; ++col) {
            for (int i = 0; i < low_h; ++i)
                temp_l[i] = data[i        * width + col];
            for (int i = 0; i < high_h; ++i)
                temp_h[i] = data[(low_h + i) * width + col];
            for (int i = 0; i < low_h; ++i) {
                int ip = (i > 0) ? i - 1 : 0;
                int ii = (i < high_h) ? i : high_h - 1;
                temp_l[i] -= (float)(CDF97_D * (temp_h[ip] + temp_h[ii]));
            }
            for (int i = 0; i < high_h; ++i) {
                int in = (i + 1 < low_h) ? i + 1 : low_h - 1;
                temp_h[i] -= (float)(CDF97_G * (temp_l[i] + temp_l[in]));
            }
            for (int i = 0; i < low_h; ++i) {
                int ip = (i > 0) ? i - 1 : 0;
                int ii = (i < high_h) ? i : high_h - 1;
                temp_l[i] -= (float)(CDF97_B * (temp_h[ip] + temp_h[ii]));
            }
            for (int i = 0; i < high_h; ++i) {
                int in = (i + 1 < low_h) ? i + 1 : low_h - 1;
                temp_h[i] -= (float)(CDF97_A * (temp_l[i] + temp_l[in]));
            }
            /* Write back: interleaved even/odd rows */
            for (int i = 0; i < low_h; ++i)
                data[(2*i)     * width + col] = temp_l[i];
            for (int i = 0; i < high_h; ++i)
                data[(2*i + 1) * width + col] = temp_h[i];
        }
        /* --- Inverse horizontal lifting (prev_w cols x prev_h rows) --- */
        if (high_w > 0)
        for (int row = 0; row < prev_h; ++row) {
            float *r = data + row * width;
            for (int j = 0; j < low_w; ++j)  temp_l[j] = r[j];
            for (int j = 0; j < high_w; ++j) temp_h[j] = r[low_w + j];
            for (int j = 0; j < low_w; ++j) {
                int jp = (j > 0) ? j - 1 : 0;
                int jj = (j < high_w) ? j : high_w - 1;
                temp_l[j] -= (float)(CDF97_D * (temp_h[jp] + temp_h[jj]));
            }
            for (int j = 0; j < high_w; ++j) {
                int jn = (j + 1 < low_w) ? j + 1 : low_w - 1;
                temp_h[j] -= (float)(CDF97_G * (temp_l[j] + temp_l[jn]));
            }
            for (int j = 0; j < low_w; ++j) {
                int jp = (j > 0) ? j - 1 : 0;
                int jj = (j < high_w) ? j : high_w - 1;
                temp_l[j] -= (float)(CDF97_B * (temp_h[jp] + temp_h[jj]));
            }
            for (int j = 0; j < high_w; ++j) {
                int jn = (j + 1 < low_w) ? j + 1 : low_w - 1;
                temp_h[j] -= (float)(CDF97_A * (temp_l[j] + temp_l[jn]));
            }
            for (int j = 0; j < low_w; ++j)  r[2*j]     = temp_l[j];
            for (int j = 0; j < high_w; ++j) r[2*j + 1] = temp_h[j];
        }
        w = prev_w; h = prev_h;
    }
    free(temp_l); free(temp_h);
}

/* --- QUANTIZATION HELPERS --- */
/* Precompute base once per channel (quality and is_chroma are channel constants). */
/* Dead-zone factor (DZ < 1.0 compensates for lost roundf-bias from float quantization). */
/* Band quantization tables: index 0=coarsest, N-1=finest. Trained by ssimulacra2 metric. */
/* Under WTPC_TUNE_PARAMS: mutable (tune_grid modifies them live). */
/* In production: const, hardcoded optimized values. */
#ifdef WTPC_TUNE_PARAMS
#define WTPC_TABLES_CONST
#else
#define WTPC_TABLES_CONST const
#endif
#define MAX_BANDS 8
/* MAX_BANDS multipliers + 1 DZ at [MAX_BANDS] */
static WTPC_TABLES_CONST float g_quant_y[MAX_BANDS+1]    = {0.41f, 0.21f, 0.18f, 0.19f, 0.27f, 0.51f, 1.17f, 3.31f, 0.56f};
static WTPC_TABLES_CONST float g_quant_c[MAX_BANDS+1]    = {0.43f, 0.26f, 0.29f, 0.42f, 0.77f, 1.55f, 3.19f, 6.89f, 0.63f};
static WTPC_TABLES_CONST float g_quant_c420[MAX_BANDS+1] = {0.20f, 0.17f, 0.20f, 0.29f, 0.53f, 1.10f, 2.16f, 4.21f, 0.61f};

static inline float compute_base(int quality) {
    float q_eff;
    if      (quality <= 30)  q_eff = 1.0f + (quality - 1) * 0.1f;
    else if (quality <= 80)  q_eff = 4.0f + (quality - 31) * 0.2f;
    else                     q_eff = 14.0f + (quality - 81) * 1.0f;
    return q_eff;
}

/* Pre-compute LL sizes from coarsest (1x1) to finest (w h). */
/* lw[0]=1 (final LL), lw[levels]=w (full). Supports up to 65536 (16 levels). */
/* Returns number of decomposition levels. */
/* 1x1 2x2 4x4 8x8 16x16 32x32 64x64 128x128 256x256 */
static inline int compute_ll_sizes(int w, int h, int lw[17], int lh[17]) {
    int tmp_w[17], tmp_h[17];
    tmp_w[0] = w; tmp_h[0] = h;
    int levels = 0, cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        cw = (cw + 1) / 2; ch = (ch + 1) / 2;
        levels++;
        tmp_w[levels] = cw;
        tmp_h[levels] = ch;
    }
    /* Reverse: coarsest (1x1) -> finest (w h) */
    for (int i = 0; i <= levels; i++) {
        lw[i] = tmp_w[levels - i];
        lh[i] = tmp_h[levels - i];
    }
    /* Pad band indices with full size (always valid for unrolled/loop checks) */
    for (int i = levels + 1; i <= 16; i++) {
        lw[i] = w;
        lh[i] = h;
    }
    return levels;
}

/* Quantization multiplier by subband level. */
/* lw[0]=1 (coarsest), lw[levels]=w (finest). */
static inline float get_quant_mult(int x, int y, const int *lw, const int *lh, const float *bands_mult) {
    for (int k = 1; k < MAX_BANDS; k++) {
        if (x < lw[k] && y < lh[k]) return bands_mult[k - 1];
    }
    return bands_mult[MAX_BANDS - 1];
}

/* --- Quantization (DEAD-ZONE SCALAR, float precision) --- */
#ifdef DEBUG_WAVELET
/* Save float array as PNG: set DC to 0, abs + sqrt scaling, normalize to 0-255. */
static void save_wavelet_png(const float *data, int w, int h, const char *path) {
    int total = w * h;
    float maxv = 0.0f;
    for (int i = 1; i < total; i++) {  /* skip DC at i=0 */
        float v = sqrtf(fabsf(data[i]));
        if (v > maxv) maxv = v;
    }
    if (maxv < 1e-6f) maxv = 1.0f;
    uint8_t *img = (uint8_t*)malloc(total);
    if (!img) return;
    for (int i = 0; i < total; i++) {
        float v = (i == 0) ? 0.0f : sqrtf(fabsf(data[i])) * 255.0f / maxv;
        if (v > 255.0f) v = 255.0f;
        img[i] = (uint8_t)v;
    }
    stbi_write_png(path, w, h, 1, img, w);
    free(img);
}
#endif
/* Band-specific minimum steps to prevent int16_t overflow. */
/* CDF 9/7 without K-scaling: per-level LL gain = 1.513. */
/* CDF 9/7 with standard K-scaling: per-level LL gain = 1.513 x K^2 = 2.0. */
#ifdef STANDARD_CDF97
#define WT_GAIN_PER_LEVEL 2.0f
#else
#define WT_GAIN_PER_LEVEL 1.513f
#endif
/* Bands 4->0 = coarsest->finest (matching get_quant_mult / quant_band). */
static void compute_safe_steps(float min_step[5], int w, int h) {
    /* Count decomposition levels */
    int levels = 0, d = w > h ? w : h;
    while (d > 1) { d = (d + 1) / 2; levels++; }
    /* band 4 (coarsest, x<lw[4]): includes LL -> max gain 1.513^levels */
    /* band 3 (x<lw[5]): level N-4 -> gain 1.513^(levels-4) */
    /* band 2 (x<lw[6]): level N-5 -> gain 1.513^(levels-5) */
    /* band 1 (x<lw[7]): level N-6 -> gain 1.513^(levels-6) */
    /* band 0 (else):    level N-7 -> gain 1.513^(levels-7) */
    static const int band_levels_delta[5] = {7, 6, 5, 4, 0};
    for (int b = 0; b < 5; b++) {
        int bl = levels - band_levels_delta[b];
        if (bl < 1) bl = 1;
        float gain = 1.0f;
        for (int i = 0; i < bl; i++) gain *= WT_GAIN_PER_LEVEL;
        min_step[b] = (510.0f * gain) / 32767.0f;
        if (min_step[b] < 1.0f) min_step[b] = 1.0f;
    }
}

static void quantize_coeffs(const float *wavelet, int16_t *quantized, int w, int h, float base, const float *bands_mult) {
    int lw[17], lh[17];
    compute_ll_sizes(w, h, lw, lh);
    float min_step[5];
    compute_safe_steps(min_step, w, h);
    float dz_factor = bands_mult[MAX_BANDS];
    /* Padded local copy of bands_mult (stack, 68 bytes) — no bounds check in loop */
    #define LUT_SZ 17
    float lb[LUT_SZ];
    for (int b = 0; b < MAX_BANDS; b++) lb[b] = bands_mult[b];
    for (int b = MAX_BANDS; b < LUT_SZ; b++) lb[b] = bands_mult[MAX_BANDS-1];
    static const int qb_lut[17] = {0,4,4,4,4,3,2,1,0,0,0,0,0,0,0,0,0};
    int i = 0;
    /* Precompute per-band step + deadzone (k=1..LUT_SZ-1): saves 1 mul+1 LUT+1 clamp+1 mul per pixel */
    float step_k[LUT_SZ], dz_k[LUT_SZ];
    for (int k = 1; k < LUT_SZ; k++) {
        float s = base * lb[k - 1];
        int qb = qb_lut[k];
        s = fmaxf(s, min_step[qb]);   // branchless SSE maxss
        step_k[k] = s;
        dz_k[k]  = s * dz_factor;
    }

    for (int y = 0; y < h; y++) {
        int k_y = MAX_BANDS;
        for (int k = 1; k < MAX_BANDS; k++) { if (y < lh[k]) { k_y = k; break; } }

        /* Segment the row by subband boundary lw[k]: within [x, seg_end) the band   */
        /* index k = max(k_x, k_y) is constant, so step/deadzone are loop-invariant. */
        /* get_quant_mult(x,y) picks the first k with x<lw[k] && y<lh[k] = max(k_x,  */
        /* k_y) since lw/lh are monotone. NOTE: do NOT shortcut k_x via floor(log2   */
        /* x)+1 even for power-of-two widths -- lw[k] != 2^k when width and height   */
        /* decompose into a different number of levels (non-square images pad the    */
        /* coarse end of lw), which silently corrupts quantization. */
        int x = 0, k_x = 1;
        while (k_x < MAX_BANDS && lw[k_x] <= 0) k_x++;
        while (x < w) {
            int seg_end = (k_x < MAX_BANDS) ? lw[k_x] : w;
            if (seg_end > w) seg_end = w;
            int k = k_x > k_y ? k_x : k_y;
            float step = step_k[k], dz = dz_k[k];
            for (; x < seg_end; x++, i++) {
                float val = wavelet[i];
                if (fabsf(val) < dz) { quantized[i] = 0; }
                else { float v = val / step; quantized[i] = (int16_t)(int)(v + copysignf(0.5f, v)); }
            }
            if (k_x < MAX_BANDS) k_x++; else break;  /* seg_end==w reached end of row */
        }
    }
    #undef LUT_SZ
}

static void dequantize_channel(const int16_t *quantized, float *f_out, int w, int h, float base, const float *bands_mult) {
    int lw[17], lh[17];
    compute_ll_sizes(w, h, lw, lh);
    float min_step[5];
    compute_safe_steps(min_step, w, h);
    #define LUT_SZ 17
    float lb[LUT_SZ];
    for (int b = 0; b < MAX_BANDS; b++) lb[b] = bands_mult[b];
    for (int b = MAX_BANDS; b < LUT_SZ; b++) lb[b] = bands_mult[MAX_BANDS-1];
    static const int qb_lut[17] = {0,4,4,4,4,3,2,1,0,0,0,0,0,0,0,0,0};
    int i = 0;
    /* Precompute per-band step (k=1..LUT_SZ-1): saves 1 mul+1 LUT+1 clamp per pixel */
    float step_k[LUT_SZ];
    for (int k = 1; k < LUT_SZ; k++) {
        float s = base * lb[k - 1];
        int qb = qb_lut[k];
        step_k[k] = fmaxf(s, min_step[qb]);
    }

    for (int y = 0; y < h; y++) {
        int k_y = MAX_BANDS;
        for (int k = 1; k < MAX_BANDS; k++) { if (y < lh[k]) { k_y = k; break; } }

        /* Same segmented row walk as quantize_coeffs (must match exactly). */
        int x = 0, k_x = 1;
        while (k_x < MAX_BANDS && lw[k_x] <= 0) k_x++;
        while (x < w) {
            int seg_end = (k_x < MAX_BANDS) ? lw[k_x] : w;
            if (seg_end > w) seg_end = w;
            int k = k_x > k_y ? k_x : k_y;
            float step = step_k[k];
            for (; x < seg_end; x++, i++)
                f_out[i] = (float)(quantized[i] * step);
            if (k_x < MAX_BANDS) k_x++; else break;
        }
    }
    #undef LUT_SZ
}

/* ===================================================================== */
/*  Huffman (JPEG-style: (run, category) + extra bits) */
/* ===================================================================== */

#define MAX_CAT 14
#define MAX_CODE_LEN     16
/* 16 leaf symbols -> at most 2*16-1 = 31 tree nodes; round up for safety */
#define MAX_HUFF_NODES   64

/* --- Category of magnitude (how many bits for |val|) --- */
static inline int category_of(int val) {
    unsigned int abs_val = (unsigned int)(val < 0 ? -val : val);
    int cat = 0;
    while (abs_val) { cat++; abs_val >>= 1; }
    if (cat > MAX_CAT) cat = MAX_CAT;
    return cat;
}

/* --- Encode value in category (JPEG-style) --- */
/* Category N: 2^N values, upper half = positive, lower half = negative. */
static inline uint32_t encode_category_bits(int value, int cat) {
    if (cat == 0) return 0;
    if (value < 0) {
        return (uint32_t)(value + (1 << cat) - 1);
    } else {
        return (uint32_t)value;
    }
}

static inline int decode_category_bits(uint32_t bits, int cat) {
    if (cat == 0) return 0;
    int threshold = 1 << (cat - 1);
    if (bits < (uint32_t)threshold) {
        return (int)bits - (1 << cat) + 1;
    } else {
        return (int)bits;
    }
}

/* --- Build Huffman tree -> code lengths --- */
static void build_huffman_codes(int *freq, int num_symbols, int *code_lens) {
    memset(code_lens, 0, num_symbols * sizeof(int));
    typedef struct {
        int freq, left, right, symbol;
    } Node;
    
    Node nodes[MAX_HUFF_NODES];
    int n = 0;
    int used[MAX_HUFF_NODES];
    memset(used, 0, sizeof(used));  /* init all */
    
    /* Build leaf nodes (symbols with freq > 0) */
    for (int i = 0; i < num_symbols; i++) {
        if (freq[i] > 0) {
            nodes[n].freq   = freq[i];
            nodes[n].symbol = i;
            nodes[n].left   = -1;
            nodes[n].right  = -1;
            n++;
        }
    }
    
    if (n == 0) return;
    if (n == 1) {
        code_lens[nodes[0].symbol] = 1;
        return;
    }
    
    int total_nodes = n;
    
    /* Merge two smallest until one root remains */
    while (1) {
        int active = 0;
        for (int i = 0; i < total_nodes; i++)
            if (!used[i]) active++;
        if (active <= 1) break;
        
        int m1 = -1, m2 = -1;
        for (int i = 0; i < total_nodes; i++) {
            if (used[i]) continue;
            if (m1 == -1 || nodes[i].freq < nodes[m1].freq) {
                m2 = m1; m1 = i;
            } else if (m2 == -1 || nodes[i].freq < nodes[m2].freq) {
                m2 = i;
            }
        }
        
        /* Create internal node */
        nodes[total_nodes].freq   = nodes[m1].freq + nodes[m2].freq;
        nodes[total_nodes].left   = m1;
        nodes[total_nodes].right  = m2;
        nodes[total_nodes].symbol = -1;
        used[total_nodes] = 0;  /* new node is active */
        
        used[m1] = 1;
        used[m2] = 1;
        total_nodes++;
    }
    
    /* Find root (only unused node) */
    int root = -1;
    for (int i = 0; i < total_nodes; i++) {
        if (!used[i]) { root = i; break; }
    }
    
    /* DFS tree traversal -> code lengths */
    int stack[MAX_HUFF_NODES];
    int depth_stack[MAX_HUFF_NODES];
    int sp = 0;
    stack[sp] = root;
    depth_stack[sp] = 0;
    sp++;
    
    while (sp > 0) {
        sp--;
        int node  = stack[sp];
        int depth = depth_stack[sp];
        
        if (nodes[node].symbol >= 0) {
            int len = depth;
            if (len == 0) len = 1;  /* single symbol edge case */
            if (len > MAX_CODE_LEN) len = MAX_CODE_LEN;
            code_lens[nodes[node].symbol] = len;
        } else {
            stack[sp] = nodes[node].right;
            depth_stack[sp] = depth + 1;
            sp++;
            stack[sp] = nodes[node].left;
            depth_stack[sp] = depth + 1;
            sp++;
        }
    }
}

static void generate_canonical_codes(int *code_lens, int num_symbols, uint32_t *huff_codes) {
    int bl_count[MAX_CODE_LEN + 2];
    memset(bl_count, 0, sizeof(bl_count));
    
    int max_len = 0;
    for (int i = 0; i < num_symbols; i++) {
        int len = code_lens[i];
        if (len > 0) {
            bl_count[len]++;
            if (len > max_len) max_len = len;
        }
    }
    
    uint32_t next_code[MAX_CODE_LEN + 2];
    uint32_t code = 0;
    /* Compute first code for each length (right-justified, MSB-first on transmit) */
    for (int bits = 1; bits <= max_len; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }
    
    for (int i = 0; i < num_symbols; i++) {
        int len = code_lens[i];
        if (len > 0) {
            huff_codes[i] = next_code[len]++;
        }
    }
}

/* --- Write Huffman table (compact: bitmap + per-symbol lengths) --- */
static int write_huffman_table(Bitstream *bs, int *code_lens, int num_symbols) {
    int start = bitstream_bits(bs);
    uint16_t bitmap = 0;
    for (int i = 0; i < num_symbols; i++)
        if (code_lens[i] > 0) bitmap |= (1u << i);
    put_bits(bs, bitmap, num_symbols);  /* 16-bit bitmap for 16 symbols */
    for (int i = 0; i < num_symbols; i++)
        if (code_lens[i] > 0)
            put_bits(bs, code_lens[i] - 1, 4);  /* length-1 (0..15) */
    return bitstream_bits(bs) - start;
}

/* --- Read Huffman table (compact: bitmap + per-symbol lengths) --- */
/* Format: 16-bit bitmap (bit i=1 means symbol i has code), */
/* then 4-bit (code_len-1) for each present symbol. */
static void read_huffman_table(Bitstream *bs, int *code_lens, int num_symbols) {
    memset(code_lens, 0, num_symbols * sizeof(int));
    uint16_t bitmap = get_bits(bs, num_symbols);
    for (int i = 0; i < num_symbols; i++) {
        if (bitmap & (1u << i)) {
            code_lens[i] = (int)get_bits(bs, 4) + 1;
        }
    }
}

static int huffman_decode_symbol(Bitstream *bs, uint32_t *huff_codes, int *code_lens, int num_symbols) {
    uint32_t code = 0;
    for (int len = 1; len <= MAX_CODE_LEN; len++) {
        code = (code << 1) | get_bits(bs, 1);
        for (int sym = 0; sym < num_symbols; sym++) {
            if (code_lens[sym] == len && huff_codes[sym] == code) {
                return sym;
            }
        }
    }
    return -1;  /* decode error */
}

/* ===================================================================== */
/*  EXP-GOLOMB MODE: Huffman for categories (16 symbols) + EG for run lengths. */
/*  Far more efficient than JPEG-style (run,cat) pairs for wavelet data */
/*  where zero runs can be 1000+ coefficients long. */
/* ===================================================================== */
#define NUM_HUFF_SYMBOLS 16
#define EOB_SYMBOL 15

static void count_frequencies(const int16_t *d, int sz, int *freq) {
    memset(freq, 0, NUM_HUFF_SYMBOLS * sizeof(int));
    for (int i = 0; i < sz; i++) {
        if (d[i] == 0) continue;
        int cat = category_of(d[i]);
        if (cat < NUM_HUFF_SYMBOLS-1) freq[cat]++;
    }
    freq[EOB_SYMBOL] = 1;
}

/* Write one channel: for each nonzero value emit (huffman_code, eg_run, extra_bits) */
static void huffman_encode_runval(Bitstream *bs, const int16_t *d, int sz,
                                   uint32_t *hc, int *cl) {
    int run = 0;
    for (int i = 0; i < sz; i++) {
        if (d[i] == 0) { run++; continue; }
        int cat = category_of(d[i]);
        put_bits(bs, hc[cat], cl[cat]);
        put_eg(bs, run);
        if (cat > 0)
            put_bits(bs, encode_category_bits(d[i], cat), cat);
        run = 0;
    }
    if (run > 0)
        put_bits(bs, hc[EOB_SYMBOL], cl[EOB_SYMBOL]);
}

static void huffman_decode_channel(Bitstream *bs, int16_t *o, int sz,
                      int *cl, uint32_t *hc) {
    int i = 0;
    while (i < sz) {
        int sym = huffman_decode_symbol(bs, hc, cl, NUM_HUFF_SYMBOLS);
        if (sym < 0 || sym == EOB_SYMBOL) { while (i < sz) o[i++] = 0; return; }
        int run = get_eg(bs);
        int end = i + run; if (end > sz) end = sz;
        while (i < end) o[i++] = 0;
        if (i >= sz) break;
        uint32_t bits = get_bits(bs, sym);
        o[i++] = (int16_t)decode_category_bits(bits, sym);
    }
}

/* Context-aware Huffman: prev non-zero category selects between 2 tables */
static void huffman_encode_ctx(Bitstream *bs, const int16_t *d, int sz,
                                uint32_t *hc0, int *cl0, uint32_t *hc1, int *cl1) {
    int run = 0, last_cat = 0;
    for (int i = 0; i < sz; i++) {
        if (d[i] == 0) { run++; continue; }
        int cat = category_of(d[i]);
        uint32_t *hc = (last_cat <= 2) ? hc0 : hc1;
        int *cl = (last_cat <= 2) ? cl0 : cl1;
        put_bits(bs, hc[cat], cl[cat]);
        put_eg(bs, run);
        if (cat > 0) put_bits(bs, encode_category_bits(d[i], cat), cat);
        run = 0; last_cat = cat;
    }
    if (run > 0) {
        uint32_t *hc = (last_cat <= 2) ? hc0 : hc1;
        int *cl = (last_cat <= 2) ? cl0 : cl1;
        put_bits(bs, hc[EOB_SYMBOL], cl[EOB_SYMBOL]);
    }
}

static void count_frequencies_ctx(const int16_t *d, int sz, int *freq0, int *freq1) {
    memset(freq0, 0, NUM_HUFF_SYMBOLS * sizeof(int));
    memset(freq1, 0, NUM_HUFF_SYMBOLS * sizeof(int));
    int last_cat = 0;
    for (int i = 0; i < sz; i++) {
        if (d[i] == 0) continue;
        int cat = category_of(d[i]);
        if (cat < NUM_HUFF_SYMBOLS-1) {
            if (last_cat <= 2) freq0[cat]++; else freq1[cat]++;
        }
        last_cat = cat;
    }
    freq0[EOB_SYMBOL] = 1; freq1[EOB_SYMBOL] = 1;
}

static int measure_encode_bits_ctx(const int16_t *d, int sz,
                                    uint32_t *hc0, int *cl0, uint32_t *hc1, int *cl1) {
    int buf_sz = sz * 3;
    uint8_t *tmp = (uint8_t*)calloc(1, buf_sz);
    Bitstream bs; bitstream_init(&bs, tmp, buf_sz);
    huffman_encode_ctx(&bs, d, sz, hc0, cl0, hc1, cl1);
    bitstream_flush(&bs);
    int bits = bitstream_bits(&bs);
    free(tmp);
    return bits;
}

/* ===================================================================== */
/*  Default Huffman tables. Auto-generated from 2816 images. Quality levels: {400,200,100,50,20,8,3} */
/*  single table: for huf_extra_ctx=0 (all coefficients, 7 levels) */
/*  t0 tables: prev-cat <= 2 (small coefficients, 3-bit selector) */
/*  t1 tables: prev-cat > 2 (large coefficients, 7 levels, 3-bit selector) */
/* ===================================================================== */
#define NUM_DEF_TABLES 7
#define NUM_DEF_T1 NUM_DEF_TABLES
#define HUFF_TBL_MASK NUM_DEF_TABLES  /* must be 2^n-1 (3,7,15,...) to work as bitmask */

/* Single-table (huf_extra_ctx=0): all coefficients */
static const uint8_t def_tables_single[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,1,2,3,4,6,7,7,0,0,0,0,0,0,0,5},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3}},
 {{0,1,2,3,4,5,6,8,8,0,0,0,0,0,0,7},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4}},
 {{0,1,2,3,4,5,6,7,8,10,10,0,0,0,0,9},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,12,0,0,11},{0,1,2,3,4,5,6,7,9,10,11,11,0,0,0,8},{0,1,2,3,4,5,6,7,9,10,11,11,0,0,0,8}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10}},
 {{0,2,2,2,3,4,5,6,7,8,9,11,12,12,0,10},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10},{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10}}
};

/* t0 tables (prev-cat <= 2) */
static const uint8_t def_tables_t0[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,1,2,3,4,6,6,0,0,0,0,0,0,0,0,5},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3}},
 {{0,1,2,3,4,5,7,7,0,0,0,0,0,0,0,6},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4}},
 {{0,1,2,3,4,5,6,7,9,9,0,0,0,0,0,8},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6}},
 {{0,1,2,3,4,5,6,7,8,9,11,11,0,0,0,10},{0,1,2,3,4,5,6,8,9,10,11,11,0,0,0,7},{0,1,2,3,4,5,6,8,9,10,11,11,0,0,0,7}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9}},
 {{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,13,13,0,9}}
};

/* t1 tables (prev-cat > 2) */
static const uint8_t def_tables_t1[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,2,2,2,3,5,6,6,0,0,0,0,0,0,0,4},{0,2,3,4,4,0,0,0,0,0,0,0,0,0,0,1},{0,2,3,4,5,5,0,0,0,0,0,0,0,0,0,1}},
 {{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5},{0,2,2,3,4,5,5,0,0,0,0,0,0,0,0,2},{0,2,2,2,4,5,5,0,0,0,0,0,0,0,0,3}},
 {{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7},{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5},{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,10,0,0,9},{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7},{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8},{0,2,2,2,3,4,5,6,7,8,10,10,0,0,0,9}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8},{0,2,2,2,3,4,5,6,7,8,10,10,0,0,0,9}}
};

/* 4:2:0 single-table (huf_extra_ctx=0): all coefficients */
static const uint8_t def_tables_single_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,1,2,3,4,6,7,7,0,0,0,0,0,0,0,5},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3}},
 {{0,1,2,3,4,5,6,8,8,0,0,0,0,0,0,7},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4}},
 {{0,1,2,3,4,5,6,7,8,10,10,0,0,0,0,9},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,12,0,0,11},{0,1,2,3,4,5,6,7,9,10,11,11,0,0,0,8},{0,1,2,3,4,5,6,7,9,10,11,11,0,0,0,8}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9}},
 {{0,2,2,2,3,4,5,6,7,8,9,11,12,12,0,10},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9},{0,1,2,3,4,5,6,7,8,10,11,12,12,0,0,9}}
};

/* 4:2:0 t0 tables (prev-cat <= 2) */
static const uint8_t def_tables_t0_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,1,2,3,4,6,6,0,0,0,0,0,0,0,0,5},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3},{0,1,2,4,5,6,6,0,0,0,0,0,0,0,0,3}},
 {{0,1,2,3,4,5,7,7,0,0,0,0,0,0,0,6},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4},{0,1,2,3,5,6,7,7,0,0,0,0,0,0,0,4}},
 {{0,1,2,3,4,5,6,7,9,9,0,0,0,0,0,8},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6},{0,1,2,3,4,5,7,8,9,9,0,0,0,0,0,6}},
 {{0,1,2,3,4,5,6,7,8,9,11,11,0,0,0,10},{0,1,2,3,4,5,6,8,9,10,11,11,0,0,0,7},{0,1,2,3,4,5,6,8,9,10,11,11,0,0,0,7}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8}},
 {{0,1,2,3,4,5,6,7,8,9,10,12,13,13,0,11},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8}},
 {{0,1,2,3,4,5,6,7,8,9,11,12,13,13,0,10},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8},{0,1,2,3,4,5,6,7,9,10,11,12,12,0,0,8}}
};

/* 4:2:0 t1 tables (prev-cat > 2) */
static const uint8_t def_tables_t1_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {
 {{0,2,2,2,3,5,6,6,0,0,0,0,0,0,0,4},{0,2,3,4,4,0,0,0,0,0,0,0,0,0,0,1},{0,2,3,4,5,5,0,0,0,0,0,0,0,0,0,1}},
 {{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5},{0,2,2,3,4,5,5,0,0,0,0,0,0,0,0,2},{0,2,2,3,4,5,5,0,0,0,0,0,0,0,0,2}},
 {{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7},{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5},{0,2,2,2,3,4,6,7,7,0,0,0,0,0,0,5}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,10,0,0,9},{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7},{0,2,2,2,3,4,5,6,8,9,9,0,0,0,0,7}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,9,0,0,0,0,8},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,9,0,0,0,0,8},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8}},
 {{0,3,2,2,3,3,4,5,6,7,8,10,11,11,0,9},{0,2,2,2,3,4,5,6,7,9,9,0,0,0,0,8},{0,2,2,2,3,4,5,6,7,9,10,10,0,0,0,8}}
};

static int measure_encode_bits(const int16_t *d, int sz, uint32_t *hc, int *cl) {
    int buf_sz = sz * 3;  /* safe upper bound */
    uint8_t *tmp = (uint8_t*)calloc(1, buf_sz);
    Bitstream bs;
    bitstream_init(&bs, tmp, buf_sz);
    huffman_encode_runval(&bs, d, sz, hc, cl);
    bitstream_flush(&bs);
    free(tmp);
    return bitstream_bits(&bs);
}

static int pick_best_table(const int16_t *d, int sz, int *freq, int ch,
                           int *clo, uint32_t *hco, int *best_bits, int is_420) {
    int best_idx = 0, best_total = 99999999;
    int cl[NUM_HUFF_SYMBOLS]; uint32_t hc[NUM_HUFF_SYMBOLS];
    const uint8_t (*tbl)[3][NUM_HUFF_SYMBOLS] = (is_420) ? def_tables_single_420 : def_tables_single;
    for (int t = 0; t < NUM_DEF_TABLES; t++) {
        /* Check that every category present in frequency has a non-zero */
        /* code length in this default table. Otherwise the table is invalid */
        /* because the decoder cannot match 0-length codes. */
        int ok = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) {
            if (freq[i] > 0 && tbl[t][ch][i] == 0)
                { ok = 0; break; }
        }
        if (!ok) continue;
        for(int i = 0; i < NUM_HUFF_SYMBOLS; i++)
            cl[i] = tbl[t][ch][i];
        generate_canonical_codes(cl, NUM_HUFF_SYMBOLS, hc);
        int bits = measure_encode_bits(d, sz, hc, cl);
        if (bits < best_total) {
            best_total = bits; best_idx = t;
            memcpy(clo, cl, sizeof(cl)); memcpy(hco, hc, sizeof(hc));
        }
    }
    memset(cl, 0, sizeof(cl));  /* ensure all entries initialized */
    build_huffman_codes(freq, NUM_HUFF_SYMBOLS, cl);
    generate_canonical_codes(cl, NUM_HUFF_SYMBOLS, hc);
    int bits = measure_encode_bits(d, sz, hc, cl);
    int cnt = 0; for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) if (cl[i] > 0) cnt++;
    int total = bits + NUM_HUFF_SYMBOLS + cnt * 4;
    if (total < best_total) {
        best_total = total; best_idx = NUM_DEF_TABLES;  /* custom table (index == NUM_DEF_TABLES) */
        memcpy(clo, cl, sizeof(cl)); memcpy(hco, hc, sizeof(hc));
    }
    if (best_bits) *best_bits = best_total;  /* total cost incl. table header for custom */
    return best_idx;
}

/* Pick best pair of context tables (t0 for prev-cat<=2, t1 for prev-cat>2). */
/* Returns codes in cl0/hc0 (table0) and cl1/hc1 (table1). */
/* t0: 0..6=default, 7=custom.  t1: 0..6=default, NUM_DEF_T1=custom. */
static void pick_best_tables_ctx(const int16_t *d, int sz, int ch, const int16_t *extra, int *t0_out, int *t1_out,
                                 int *cl0, uint32_t *hc0, int *cl1, uint32_t *hc1, int *best_total, int is_420) {
    int freq0[NUM_HUFF_SYMBOLS], freq1[NUM_HUFF_SYMBOLS];
    count_frequencies_ctx(d, sz, freq0, freq1);
    if (extra) {
        int fe0[NUM_HUFF_SYMBOLS], fe1[NUM_HUFF_SYMBOLS];
        count_frequencies_ctx(extra, sz, fe0, fe1);
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) { freq0[i] += fe0[i]; freq1[i] += fe1[i]; }
    }

    int best_t0 = 0, best_t1 = 0, best_bits = 99999999;
    int cl_t[NUM_HUFF_SYMBOLS]; uint32_t hc_t[NUM_HUFF_SYMBOLS];
    const uint8_t (*t0_tbl)[3][NUM_HUFF_SYMBOLS] = is_420 ? def_tables_t0_420 : def_tables_t0;
    const uint8_t (*t1_tbl)[3][NUM_HUFF_SYMBOLS] = is_420 ? def_tables_t1_420 : def_tables_t1;

    /* Try all default table pairs (t0=0..6, t1=0..6) */
    for (int t0 = 0; t0 < NUM_DEF_TABLES; t0++) {
        int ok0 = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++)
            if (freq0[i] > 0 && t0_tbl[t0][ch][i] == 0) { ok0 = 0; break; }
        if (!ok0) continue;
        int cl0_t[NUM_HUFF_SYMBOLS]; uint32_t hc0_t[NUM_HUFF_SYMBOLS];
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) cl0_t[i] = t0_tbl[t0][ch][i];
        generate_canonical_codes(cl0_t, NUM_HUFF_SYMBOLS, hc0_t);
        for (int t1 = 0; t1 < NUM_DEF_T1; t1++) {
            int ok1 = 1;
            for (int i = 0; i < NUM_HUFF_SYMBOLS; i++)
                if (freq1[i] > 0 && t1_tbl[t1][ch][i] == 0) { ok1 = 0; break; }
            if (!ok1) continue;
            for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) cl_t[i] = t1_tbl[t1][ch][i];
            generate_canonical_codes(cl_t, NUM_HUFF_SYMBOLS, hc_t);
            int bits = measure_encode_bits_ctx(d, sz, hc0_t, cl0_t, hc_t, cl_t);
            if (bits < best_bits) {
                best_bits = bits; best_t0 = t0; best_t1 = t1;
                memcpy(cl0, cl0_t, sizeof(cl0_t)); memcpy(hc0, hc0_t, sizeof(hc0_t));
                memcpy(cl1, cl_t, sizeof(cl_t)); memcpy(hc1, hc_t, sizeof(hc_t));
            }
        }
    }

    /* Build custom tables from frequencies */
    int cl0_c[NUM_HUFF_SYMBOLS] = {0}, cl1_c[NUM_HUFF_SYMBOLS] = {0};
    uint32_t hc0_c[NUM_HUFF_SYMBOLS], hc1_c[NUM_HUFF_SYMBOLS];
    build_huffman_codes(freq0, NUM_HUFF_SYMBOLS, cl0_c);
    generate_canonical_codes(cl0_c, NUM_HUFF_SYMBOLS, hc0_c);
    build_huffman_codes(freq1, NUM_HUFF_SYMBOLS, cl1_c);
    generate_canonical_codes(cl1_c, NUM_HUFF_SYMBOLS, hc1_c);
    int hdr0 = 0; for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) if (cl0_c[i] > 0) hdr0++;
    int hdr1 = 0; for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) if (cl1_c[i] > 0) hdr1++;
    int hdr0_bits = NUM_HUFF_SYMBOLS + hdr0 * 4;  /* bitmap + code lengths */
    int hdr1_bits = NUM_HUFF_SYMBOLS + hdr1 * 4;

    /* custom t0 + default t1 */
    for (int t1 = 0; t1 < NUM_DEF_T1; t1++) {
        int ok1 = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++)
            if (freq1[i] > 0 && t1_tbl[t1][ch][i] == 0) { ok1 = 0; break; }
        if (!ok1) continue;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) cl_t[i] = t1_tbl[t1][ch][i];
        generate_canonical_codes(cl_t, NUM_HUFF_SYMBOLS, hc_t);
        int bits = measure_encode_bits_ctx(d, sz, hc0_c, cl0_c, hc_t, cl_t) + hdr0_bits;
        if (bits < best_bits) {
            best_bits = bits; best_t0 = NUM_DEF_TABLES; best_t1 = t1;
            memcpy(cl0, cl0_c, sizeof(cl0_c)); memcpy(hc0, hc0_c, sizeof(hc0_c));
            memcpy(cl1, cl_t, sizeof(cl_t)); memcpy(hc1, hc_t, sizeof(hc_t));
        }
    }

    /* default t0 + custom t1 */
    for (int t0 = 0; t0 < NUM_DEF_TABLES; t0++) {
        int ok0 = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++)
            if (freq0[i] > 0 && t0_tbl[t0][ch][i] == 0) { ok0 = 0; break; }
        if (!ok0) continue;
        int cl0_t[NUM_HUFF_SYMBOLS]; uint32_t hc0_t[NUM_HUFF_SYMBOLS];
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) cl0_t[i] = t0_tbl[t0][ch][i];
        generate_canonical_codes(cl0_t, NUM_HUFF_SYMBOLS, hc0_t);
        int bits = measure_encode_bits_ctx(d, sz, hc0_t, cl0_t, hc1_c, cl1_c) + hdr1_bits;
        if (bits < best_bits) {
            best_bits = bits; best_t0 = t0; best_t1 = NUM_DEF_T1;
            memcpy(cl0, cl0_t, sizeof(cl0_t)); memcpy(hc0, hc0_t, sizeof(hc0_t));
            memcpy(cl1, cl1_c, sizeof(cl1_c)); memcpy(hc1, hc1_c, sizeof(hc1_c));
        }
    }

    /* both custom */
    {
        int bits = measure_encode_bits_ctx(d, sz, hc0_c, cl0_c, hc1_c, cl1_c) + hdr0_bits + hdr1_bits;
        if (bits < best_bits) {
            best_t0 = NUM_DEF_TABLES; best_t1 = NUM_DEF_T1;
            memcpy(cl0, cl0_c, sizeof(cl0_c)); memcpy(hc0, hc0_c, sizeof(hc0_c));
            memcpy(cl1, cl1_c, sizeof(cl1_c)); memcpy(hc1, hc1_c, sizeof(hc1_c));
        }
    }

    *t0_out = best_t0; *t1_out = best_t1;
    if (best_total) *best_total = best_bits;
}

static void huffman_decode_ctx(Bitstream *bs, int16_t *o, int sz,
                                int *cl0, uint32_t *hc0, int *cl1, uint32_t *hc1) {
    int i = 0, last_cat = 0;
    while (i < sz) {
        int *cl = (last_cat <= 2) ? cl0 : cl1;
        uint32_t *hc = (last_cat <= 2) ? hc0 : hc1;
        int sym = huffman_decode_symbol(bs, hc, cl, NUM_HUFF_SYMBOLS);
        if (sym < 0 || sym == EOB_SYMBOL) { while (i < sz) o[i++] = 0; return; }
        int run = get_eg(bs);
        int end = i + run; if (end > sz) end = sz;
        while (i < end) o[i++] = 0;
        if (i >= sz) break;
        uint32_t bits = get_bits(bs, sym);
        o[i++] = (int16_t)decode_category_bits(bits, sym);
        last_cat = category_of(o[i-1]);
    }
}

/* ===================================================================== */
/*  EBCOT-lite: bit-plane coding with context from 8 neighbors + BAC */
/*  Based on JPEG 2000 EBCOT, simplified (single pass per bit-plane). */
/* ===================================================================== */

/* --- Binary arithmetic coder (WNC bit-oriented, proven correct) --- */
/* BAC_CODE_BITS controls interval precision. It works for any value up to 30 */
/* (lo/hi/code are uint32_t; renorm shifts must stay within 32 bits, and */
/* bacm_p0 uses uint64_t intermediates). Higher precision only marginally helps */
/* (~2-3 bytes/file at ~1.4 KB targets) because the probability model itself is */
/* limited to ~14-bit counts; 24 captures essentially all of the available gain. */
#define BAC_CODE_BITS 24
#define BAC_TOP ((1u << BAC_CODE_BITS) - 1)
#define BAC_HALF (BAC_TOP / 2 + 1)
#define BAC_Q1 (BAC_TOP / 4 + 1)
#define BAC_Q3 (3 * BAC_Q1)

/* Adaptive probability model (count-based, implicit EMA via rescaling) */
typedef struct { uint16_t c0, c1; } BacModel;

static void bacm_init_ctx(BacModel *m, int ctx) {
    if (ctx < 10) {  /* CTX_SIG: significance contexts */
        static const uint16_t priors[10][2] = {
            {256,1},{128,1},{64,1},{32,1},{16,1},  /* 0-4: no parent */
            {32,4},{16,4},{8,4},{4,4},{1,4},  /* 5-9: parent sig */
        };
        m->c0 = priors[ctx][0]; m->c1 = priors[ctx][1];
    } else {
        m->c0 = m->c1 = 1;  /* sign & refinement: neutral 50/50 */
    }
}

static uint32_t bacm_p0(BacModel *m) {
    uint32_t t = (uint32_t)m->c0 + (uint32_t)m->c1;
    if (t > 0x3FF0) { m->c0 = (m->c0 >> 1) | 1; m->c1 = (m->c1 >> 1) | 1; t = m->c0 + m->c1; }
    uint32_t p = (uint32_t)(((uint64_t)m->c0 * (BAC_TOP + 1)) / t);
    return p < 1 ? 1 : (p > BAC_TOP ? BAC_TOP : p);
}

static void bacm_update(BacModel *m, int bit) {
    if (bit) m->c1++; else m->c0++;
    if ((uint32_t)m->c0 + m->c1 > 0x3FF0) { m->c0 = (m->c0>>1)|1; m->c1 = (m->c1>>1)|1; }
}

/* --- WNC Encoder (outputs bits into byte buffer) --- */
typedef struct {
    uint8_t *out;
    uint32_t lo, hi;
    int pos, sz, follow, bbuf, nbits;
} BacEnc;

static void bac_putb(BacEnc *e, int b) {
    e->bbuf = (e->bbuf << 1) | b;
    if (++e->nbits == 8) { if (e->pos < e->sz) e->out[e->pos++] = (uint8_t)e->bbuf; e->nbits = 0; e->bbuf = 0; }
}

static void bac_putb_follow(BacEnc *e, int b) {
    bac_putb(e, b);
    while(e->follow > 0) { bac_putb(e, !b); e->follow--; }
}

static void bac_init_enc(BacEnc *e, uint8_t *buf, int sz) {
    e->out = buf; e->pos = 0; e->sz = sz; e->lo=0;
    e->hi = BAC_TOP; e->follow = 0; e->bbuf = 0; e->nbits = 0;
}

static void bac_encode(BacEnc *e, int bit, BacModel *m) {
    uint32_t p0 = bacm_p0(m), range=e->hi-e->lo+1;
    if (bit) e->lo = e->lo + (uint32_t)(((uint64_t)range*p0)/(BAC_TOP + 1));
    else     e->hi = e->lo + (uint32_t)(((uint64_t)range*p0)/(BAC_TOP + 1)) - 1;
    bacm_update(m, bit);
    for(;;) {
        if(e->hi < BAC_HALF) { bac_putb_follow(e, 0); }
        else if(e->lo >= BAC_HALF) { bac_putb_follow(e, 1); e->lo -= BAC_HALF; e->hi-=BAC_HALF;}
        else if(e->lo >= BAC_Q1 && e->hi < BAC_Q3) { e->follow++; e->lo -= BAC_Q1; e->hi -= BAC_Q1;}
        else break;
        e->lo <<= 1; e->hi = (e->hi << 1) | 1;
    }
}

static int bac_flush_enc(BacEnc *e) {
    e->follow++;
    bac_putb_follow(e, e->lo < BAC_Q1 ? 0 : 1);
    if(e->nbits > 0 && e->pos < e->sz) e->out[e->pos++] = (uint8_t)(e->bbuf << (8 - e->nbits));
    return e->pos;
}

/* --- WNC Decoder (reads bits from byte buffer) --- */
typedef struct {
    const uint8_t *in;
    uint32_t lo, hi, code;
    int pos, sz, bbuf, nbits;
} BacDec;

static int bac_getb(BacDec *d) {
    if(d->nbits == 0) {
        if(d->pos < d->sz) { d->bbuf = d->in[d->pos++]; d->nbits = 8; } else return 0;
    }
    int b = (d->bbuf >> 7) & 1;
    d->bbuf <<= 1;
    d->nbits--;
    return b;
}
static void bac_init_dec(BacDec *d, const uint8_t *buf, int sz) {
    d->in = buf; d->pos = 0; d->sz = sz; d->lo = 0; d->hi = BAC_TOP; d->code = 0;
    d->bbuf = 0; d->nbits = 0;
    for(int i = 0; i < BAC_CODE_BITS; i++) d->code = (d->code << 1) | bac_getb(d);
}
static int bac_decode(BacDec *d, BacModel *m) {
    uint32_t p0 = bacm_p0(m), range = d->hi - d->lo + 1;
    uint32_t split = d->lo + (uint32_t)(((uint64_t)range*p0)/(BAC_TOP + 1));
    int bit;
    if(d->code < split) {
        bit = 0;
        d->hi = split - 1;
    } else {
        bit = 1;
        d->lo = split;
    }
    bacm_update(m, bit);
    for(;;){
        if(d->hi < BAC_HALF) {}
        else if(d->lo >= BAC_HALF){ d->lo -= BAC_HALF; d->hi -= BAC_HALF; d->code -= BAC_HALF; }
        else if(d->lo >= BAC_Q1 && d->hi < BAC_Q3) { d->lo -= BAC_Q1; d->hi -= BAC_Q1; d->code -= BAC_Q1; }
        else break;
        d->lo <<= 1;
        d->hi = (d->hi << 1) | 1;
        d->code = (d->code << 1) | bac_getb(d);
    }
    return bit;
}

/* --- EBCOT context models --- */
/* Significance: 5 levels (0-4 neighbors) x 2 (parent coefficient not significant / significant) */
#define CTX_SIG 10
/* Sign coding: 5 contexts (average sign of neighbors mapped to 0-4) */
#define CTX_SGN 5
/* Refinement coding: 8 contexts (0-3 sig neighbors x first/subsequent refinement) */
#define CTX_REF 8
#define TOTAL_CTX (CTX_SIG + CTX_SGN + CTX_REF)

/* Per-coefficient state for EBCOT */
/* We store significance as a bitmask: bit 0 = significant, bits 1-7 = unused */
/* For simplicity use a uint8_t array: 0=not significant, 1=significant */

/* Encode one channel using EBCOT-lite into an existing BAC encoder. */
/* Returns number of bit-planes (bp). */
static uint8_t ebcot_encode_channel(BacEnc *e, const int16_t *coeffs, int w, int h) {
    int total = w * h;
    if (total <= 0) return 0;
    /* Find max absolute value to determine bit-planes */
    int max_val = 0;
    for (int i = 0; i < total; i++) {
        int av = abs(coeffs[i]);
        if (av > max_val) max_val = av;
    }
    int bp = 0;
    while (max_val > 0) { bp++; max_val >>= 1; }
    if (bp == 0) bp = 1;

    /* Context models */
    BacModel models[TOTAL_CTX];
    for (int i = 0; i < TOTAL_CTX; i++) bacm_init_ctx(&models[i], i);

    /* Significance map */
    uint8_t *sig = (uint8_t*)calloc((size_t)total, 1);
    if (!sig) return 0;
    /* Neighbour-significance counts, maintained incrementally: nsig[i] = number  */
    /* of the 8 neighbours of i that are already significant. Significance is     */
    /* monotone (0->1, never reset), so instead of rescanning the 3x3 window for  */
    /* every coefficient of every bit-plane we bump the 8 neighbours' counts once */
    /* when a coefficient becomes significant. Reading nsig[i] (clamped to 4) is  */
    /* bit-identical to the old fresh sum. */
    uint8_t *nsig = (uint8_t*)calloc((size_t)total, 1);
    if (!nsig) { free(sig); return 0; }

    /* For each bit-plane (MSB first) */
    for (int b = bp - 1; b >= 0; b--) {
        int bit_mask = 1 << b;
        for (int i = 0, x = 0, y = 0; i < total; i++, x++) {
            if (x == w) { x = 0; y++; }
            int16_t v = coeffs[i];

            int ctx_sig = nsig[i];
            if (ctx_sig > 4) ctx_sig = 4;

            if (!sig[i]) {
                /* Parent context: inter-scale correlation - coefficient at (x>>1,y>>1) */
                /* in the next coarser subband. If parent is already significant, */
                /* children are much more likely to become significant. */
                int sctx = ctx_sig;
                int px = x >> 1, py = y >> 1;
                if ((x > 0 || y > 0) && sig[py * w + px]) sctx += 5;
                int is_sig = (abs(v) & bit_mask) != 0;
                bac_encode(e, is_sig, &models[sctx]);
                if (is_sig) {
                    sig[i] = 1;
                    /* Bump neighbour counts (monotone; in-bounds only, matching the */
                    /* old border-skipping fresh sum). */
                    int y0 = y > 0, y1 = y + 1 < h, x0 = x > 0, x1 = x + 1 < w;
                    if (y0) { if (x0) nsig[i-w-1]++; nsig[i-w]++; if (x1) nsig[i-w+1]++; }
                    if (x0) nsig[i-1]++;
                    if (x1) nsig[i+1]++;
                    if (y1) { if (x0) nsig[i+w-1]++; nsig[i+w]++; if (x1) nsig[i+w+1]++; }
                    int ctx_sgn = 2, sgn_sum = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && nx < w && ny >= 0 && ny < h && sig[ny * w + nx])
                                sgn_sum += (coeffs[ny * w + nx] > 0) ? 1 : -1;
                        }
                    }
                    if (sgn_sum < -1) ctx_sgn = 0;
                    else if (sgn_sum < 0) ctx_sgn = 1;
                    else if (sgn_sum == 0) ctx_sgn = 2;
                    else if (sgn_sum <= 1) ctx_sgn = 3;
                    else ctx_sgn = 4;
                    bac_encode(e, (v < 0) ? 0 : 1, &models[CTX_SIG + ctx_sgn]);
                }
            } else {
                /* Refinement: 8 contexts = 4 neighbor-count levels x first/subsequent */
                /* 'first' is true when the coefficient became significant in plane b+1 */
                int ref_bit = (abs(v) & bit_mask) != 0;
                int first = ((unsigned int)abs(v) >> (b + 1)) == 1;
                int nb = ctx_sig; if (nb > 3) nb = 3;
                int ctx_ref = nb + (first ? 0 : 4);
                bac_encode(e, ref_bit, &models[CTX_SIG + CTX_SGN + ctx_ref]);
            }
        }
    }
    free(sig);
    free(nsig);
    return (uint8_t)bp;
}

/* Decode one channel from a shared BAC decoder. */
/* coeffs: pre-allocated output (calloc'd), bp: number of bit-planes. */
static void ebcot_decode_channel(BacDec *d, int16_t *coeffs, int w, int h, int bp) {
    int total = w * h;

    BacModel models[TOTAL_CTX];
    for (int i = 0; i < TOTAL_CTX; i++) bacm_init_ctx(&models[i], i);

    uint8_t *sig = (uint8_t*)calloc((size_t)(total > 0 ? total : 0), 1);
    if (!sig) return;
    /* Incremental neighbour-significance counts (see ebcot_encode_channel). */
    uint8_t *nsig = (uint8_t*)calloc((size_t)(total > 0 ? total : 0), 1);
    if (!nsig) { free(sig); return; }

    for (int b = bp - 1; b >= 0; b--) {
        int bit_mask = 1 << b;
        for (int i = 0, x = 0, y = 0; i < total; i++, x++) {
            if (x == w) { x = 0; y++; }
            int ctx_sig = nsig[i];
            if (ctx_sig > 4) ctx_sig = 4;

            if (!sig[i]) {
                /* Parent context: inter-scale correlation */
                int sctx = ctx_sig;
                int px = x >> 1, py = y >> 1;
                if ((x > 0 || y > 0) && sig[py * w + px]) sctx += 5;
                int is_sig = bac_decode(d, &models[sctx]);
                if (is_sig) {
                    sig[i] = 1;
                    int y0 = y > 0, y1 = y + 1 < h, x0 = x > 0, x1 = x + 1 < w;
                    if (y0) { if (x0) nsig[i-w-1]++; nsig[i-w]++; if (x1) nsig[i-w+1]++; }
                    if (x0) nsig[i-1]++;
                    if (x1) nsig[i+1]++;
                    if (y1) { if (x0) nsig[i+w-1]++; nsig[i+w]++; if (x1) nsig[i+w+1]++; }
                    int ctx_sgn = 2, sgn_sum = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && nx < w && ny >= 0 && ny < h && sig[ny * w + nx])
                                sgn_sum += (coeffs[ny * w + nx] > 0) ? 1 : -1;
                        }
                    }
                    if (sgn_sum < -1) ctx_sgn = 0;
                    else if (sgn_sum < 0) ctx_sgn = 1;
                    else if (sgn_sum == 0) ctx_sgn = 2;
                    else if (sgn_sum <= 1) ctx_sgn = 3;
                    else ctx_sgn = 4;
                    int sign_pos = bac_decode(d, &models[CTX_SIG + ctx_sgn]);
                    int mag = bit_mask;
                    if (sign_pos) coeffs[i] = mag;
                    else          coeffs[i] = -mag;
                }
            } else {
                /* Refinement: 8 contexts = 4 neighbor-count levels x first/subsequent */
                int first = ((unsigned int)abs(coeffs[i]) >> (b + 1)) == 1;
                int nb = ctx_sig; if (nb > 3) nb = 3;
                int ctx_ref = nb + (first ? 0 : 4);
                int ref_bit = bac_decode(d, &models[CTX_SIG + CTX_SGN + ctx_ref]);
                if (ref_bit) {
                    int av = abs(coeffs[i]);
                    coeffs[i] = (coeffs[i] > 0) ? (int16_t)(av | bit_mask) : (int16_t)(-(av | bit_mask));
                }
            }
        }
    }

    free(sig);
    free(nsig);
}

/* ===================================================================== */
/*  WTP header helpers (shared between EBCOT and Huffman paths) */
/* ===================================================================== */

static int wtp_header_size(int w, int h, int quality) {
    int qsz = (quality <= 255) ? 1 : 2;
    int wide = (w > 256 || h > 256);
    return 4 + 1 + 1 + qsz + (wide ? 2 : 0);
}

static int write_wtp_header(uint8_t *out, int pos, uint8_t flags, int w, int h, int quality, int chroma_420, int has_alpha) {
    int wide = (w > 256 || h > 256);
    if (chroma_420)     flags |= 1 << 0;
    if (quality > 255)  flags |= 1 << 2;
    if (wide)           flags |= 1 << 3;
    if (has_alpha)      flags |= 1 << 4;
    memcpy(out + pos, "WTP", 3); out[pos+3] = flags; pos += 4;
    out[pos++] = (uint8_t)((w - 1) & 0xFF);
    out[pos++] = (uint8_t)((h - 1) & 0xFF);
    if (quality > 255) { put_u16(out + pos, (uint16_t)quality); pos += 2; }
    else               { out[pos++] = (uint8_t)quality; }
    if (wide) { out[pos++] = (uint8_t)((w - 1) >> 8); out[pos++] = (uint8_t)((h - 1) >> 8); }
    return pos;
}

/* ===================================================================== */
/*  Top-level EBCOT encode/decode (all 3 channels) */
/* ===================================================================== */

/* EBCOT encode from already-quantized coefficients -> malloc'd WTP buffer */
static uint8_t *ebcot_pack(const int16_t *q_y, const int16_t *q_u, const int16_t *q_v, const int16_t *q_a, wtpc_enc_info *info,
                            int w, int h, int cw, int ch,
                            int quality, int chroma_420, int *out_size) {
    /* Worst case: 3 channels x (non-420) x 16 bits/coeff x 3 bytes margin */
    /*             = total coeffs x 48 bits -> 6 bytes/coeff. total*8 for safety. */
    int buf_sz = w * h * 8 + 4096;
    int hdr_sz = wtp_header_size(w, h, quality) + (q_a ? 3 : 2);  /* packed bp = 15 or 20 bits */
    uint8_t *out = malloc(hdr_sz + buf_sz);
    if (!out) return NULL;

    BacEnc e;
    bac_init_enc(&e, out + hdr_sz, buf_sz);

    uint8_t bp_y = ebcot_encode_channel(&e, q_y, w, h);
    uint8_t bp_u = ebcot_encode_channel(&e, q_u, cw, ch);
    uint8_t bp_v = ebcot_encode_channel(&e, q_v, cw, ch);
    uint8_t bp_a = 0;
    if (q_a) bp_a = ebcot_encode_channel(&e, q_a, w, h);
    if (!bp_y || !bp_u || !bp_v || (q_a && !bp_a)) { free(out); return NULL; }
    int data_sz = bac_flush_enc(&e);

    uint8_t flags = 1 << 1;  /* is_ebcot = 1 */
    int pos = write_wtp_header(out, 0, flags, w, h, quality, chroma_420, q_a != NULL);
    /* Pack bp: 5 bits each (max 16) into 2 or 3 bytes */
    if (q_a) {
        uint32_t p = bp_y | ((uint32_t)bp_u << 5) | ((uint32_t)bp_v << 10) | ((uint32_t)bp_a << 15);
        out[pos++] = (uint8_t)(p & 0xFF); out[pos++] = (uint8_t)((p >> 8) & 0xFF); out[pos++] = (uint8_t)(p >> 16);
    } else {
        uint16_t p = (uint16_t)(bp_y | (bp_u << 5) | (bp_v << 10));
        out[pos++] = (uint8_t)(p & 0xFF); out[pos++] = (uint8_t)(p >> 8);
    }

    if (out_size) *out_size = hdr_sz + data_sz;
    if (info) {
        info->ebcot = 1; info->encoded_bytes = hdr_sz + data_sz; info->result_q = quality;
    }
    return out;
}

static uint8_t *ebcot_decode_mem(const uint8_t *data, int data_len, int pos, int w, int h, int quality, int is_420, int alpha) {
    int total = w * h;
    int cw = is_420 ? (w+1)/2 : w, ch = is_420 ? (h+1)/2 : h, ctotal = cw * ch;

    /* Read packed bp: 5 bits each from 2 or 3 bytes */
    uint8_t bp_y, bp_u, bp_v, bp_a = 0;
    if (alpha) {
        uint32_t p = data[pos] | ((uint32_t)data[pos+1] << 8) | ((uint32_t)data[pos+2] << 16);
        bp_y = (uint8_t)(p & 0x1F); bp_u = (uint8_t)((p >> 5) & 0x1F);
        bp_v = (uint8_t)((p >> 10) & 0x1F); bp_a = (uint8_t)((p >> 15) & 0x1F);
        pos += 3;
    } else {
        uint16_t p = data[pos] | ((uint16_t)data[pos+1] << 8);
        bp_y = (uint8_t)(p & 0x1F); bp_u = (uint8_t)((p >> 5) & 0x1F); bp_v = (uint8_t)((p >> 10) & 0x1F);
        pos += 2;
    }

    /* Allocate coefficient arrays */
    int16_t *q_y = calloc(total, sizeof(int16_t));
    int16_t *q_u = calloc(ctotal, sizeof(int16_t));
    int16_t *q_v = calloc(ctotal, sizeof(int16_t));
    int16_t *q_a = NULL;
    if (alpha) q_a = calloc(total, sizeof(int16_t));
    if (!q_y || !q_u || !q_v || (alpha && !q_a)) { free(q_y); free(q_u); free(q_v); free(q_a); return NULL; }

    /* One shared BAC decoder for all channels */
    BacDec d;
    bac_init_dec(&d, data + pos, data_len - pos);

    ebcot_decode_channel(&d, q_y, w, h, bp_y);
    ebcot_decode_channel(&d, q_u, cw, ch, bp_u);
    ebcot_decode_channel(&d, q_v, cw, ch, bp_v);
    if (alpha) ebcot_decode_channel(&d, q_a, w, h, bp_a);

    /* Dequantize + inverse wavelet */
    float *y_f = (float*)malloc(total * sizeof(float));
    float *u_s = (float*)malloc(ctotal * sizeof(float));
    float *v_s = (float*)malloc(ctotal * sizeof(float));
    float *a_f = alpha ? (float*)malloc(total * sizeof(float)) : NULL;
    dequantize_channel(q_y, y_f, w, h, compute_base(quality), g_quant_y);
    dequantize_channel(q_u, u_s, cw, ch, compute_base(quality), is_420 ? g_quant_c420 : g_quant_c);
    dequantize_channel(q_v, v_s, cw, ch, compute_base(quality), is_420 ? g_quant_c420 : g_quant_c);
    if (alpha) dequantize_channel(q_a, a_f, w, h, compute_base(quality), g_quant_y);
    free(q_y); free(q_u); free(q_v); free(q_a);

    cdf97_inverse_2d(y_f, w, h);
    cdf97_inverse_2d(u_s, cw, ch);
    cdf97_inverse_2d(v_s, cw, ch);
    if (alpha) cdf97_inverse_2d(a_f, w, h);

    float *u_f, *v_f;
    if (is_420) {
        u_f = malloc(total*sizeof(float)); v_f = malloc(total*sizeof(float));
        chroma_up_420(u_s, cw, ch, u_f, w, h);
        chroma_up_420(v_s, cw, ch, v_f, w, h);
    } else { u_f = u_s; v_f = v_s; }

    int comp = alpha ? 4 : 3;
    uint8_t *out_img = (uint8_t*)malloc(total * comp);
    if (!out_img) { free(y_f); free(u_s); free(v_s); if (is_420) { free(u_f); free(v_f); } free(a_f); return NULL; }
    if (alpha) {
        for (int i = 0; i < total; ++i) {
            yuv_to_rgb(y_f[i], u_f[i], v_f[i], &out_img[i*4], &out_img[i*4+1], &out_img[i*4+2]);
            int av = (int)(a_f[i] + 128.5f); if (av < 0) av = 0; if (av > 255) av = 255;
            out_img[i*4+3] = (uint8_t)av;
        }
    } else {
        for (int i = 0; i < total; ++i)
            yuv_to_rgb(y_f[i], u_f[i], v_f[i], &out_img[i*3], &out_img[i*3+1], &out_img[i*3+2]);
    }

    free(y_f); free(u_s); free(v_s); free(a_f);
    if (is_420) { free(u_f); free(v_f); }
    return out_img;
}

/* Huffman encode from already-quantized coefficients -> malloc'd WTP buffer */
static unsigned char *huffman_pack(const int16_t *q_y, const int16_t *q_u, const int16_t *q_v, const int16_t *q_a, wtpc_enc_info *info,
                                    int w, int h, int cw, int ch,
                                    int quality, int chroma_420, int huf_extra_ctx, int *out_size) {
    int total = w * h, ctotal = cw * ch;
    int has_alpha = (q_a != NULL);
    int hdr_sz = wtp_header_size(w, h, quality) + 1 + (huf_extra_ctx ? 1 : 0);  /* +1=tables, +1=extra_tables */
    unsigned char *out = (unsigned char*)malloc(total * 4 * 3 + 4096);
    if (!out) return NULL;

    Bitstream bs;
    bitstream_init(&bs, out + hdr_sz, total * 4 * 3 + 4096 - hdr_sz);
    int t_y_size = 0, t_u_size = 0, t_v_size = 0;
    int t_y0, t_y1 = 0, t_u0, t_u1 = 0, t_v0, t_v1 = 0;

    /* Shared tables: cl0/hc0/cl1/hc1 reused for Y->Alpha->U->V */
    int freq[NUM_HUFF_SYMBOLS];
    int cl0[NUM_HUFF_SYMBOLS], cl1[NUM_HUFF_SYMBOLS];
    uint32_t hc0[NUM_HUFF_SYMBOLS], hc1[NUM_HUFF_SYMBOLS];
    /* Saved U tables for possible V-sharing */
    int cl_u0[NUM_HUFF_SYMBOLS], cl_u1[NUM_HUFF_SYMBOLS];
    uint32_t hc_u0[NUM_HUFF_SYMBOLS], hc_u1[NUM_HUFF_SYMBOLS];
    int shared_v = 0;

    /* Y (alpha reuses Y's codes immediately after) */
    if (huf_extra_ctx) {
        pick_best_tables_ctx(q_y, total, 0, q_a, &t_y0, &t_y1, cl0, hc0, cl1, hc1, NULL, chroma_420);
        put_bits(&bs, (t_y1 >> 2) & 1, 1);  /* t1_y hi bit as first bit in bitstream */
    } else {
        count_frequencies(q_y, total, freq);
        if (has_alpha) {
            int fa[NUM_HUFF_SYMBOLS];
            count_frequencies(q_a, total, fa);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq[s] += fa[s];
        }
        t_y0 = pick_best_table(q_y, total, freq, 0, cl0, hc0, NULL, chroma_420);
    }
    if (t_y0 == NUM_DEF_TABLES) t_y_size += write_huffman_table(&bs, cl0, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx && t_y1 == NUM_DEF_T1) t_y_size += write_huffman_table(&bs, cl1, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx) huffman_encode_ctx(&bs, q_y, total, hc0, cl0, hc1, cl1);
    else               huffman_encode_runval(&bs, q_y, total, hc0, cl0);

    /* Alpha: reuse Y's codes (still in cl0/hc0/cl1/hc1) */
    if (has_alpha) {
        if (huf_extra_ctx) huffman_encode_ctx(&bs, q_a, total, hc0, cl0, hc1, cl1);
        else               huffman_encode_runval(&bs, q_a, total, hc0, cl0);
    }

    /* U (overwrites cl0/hc0/cl1/hc1 - Y's codes no longer needed) */
    if (huf_extra_ctx) {
        pick_best_tables_ctx(q_u, ctotal, 1, NULL, &t_u0, &t_u1, cl0, hc0, cl1, hc1, NULL, chroma_420);
    } else {
        count_frequencies(q_u, ctotal, freq);
        t_u0 = pick_best_table(q_u, ctotal, freq, 1, cl0, hc0, NULL, chroma_420);
    }
    if (t_u0 == NUM_DEF_TABLES) t_u_size += write_huffman_table(&bs, cl0, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx && t_u1 == NUM_DEF_T1) t_u_size += write_huffman_table(&bs, cl1, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx) huffman_encode_ctx(&bs, q_u, ctotal, hc0, cl0, hc1, cl1);
    else               huffman_encode_runval(&bs, q_u, ctotal, hc0, cl0);
    /* Save U's codes for potential V-sharing */
    memcpy(cl_u0, cl0, sizeof(cl0)); memcpy(hc_u0, hc0, sizeof(hc0));
    if (huf_extra_ctx) { memcpy(cl_u1, cl1, sizeof(cl1)); memcpy(hc_u1, hc1, sizeof(hc1)); }

    /* V: pick own tables, then check if sharing U's works better */
    if (huf_extra_ctx) {
        int fv0[NUM_HUFF_SYMBOLS], fv1[NUM_HUFF_SYMBOLS];
        int bv;
        pick_best_tables_ctx(q_v, ctotal, 2, NULL, &t_v0, &t_v1, cl0, hc0, cl1, hc1, &bv, chroma_420);
        count_frequencies_ctx(q_v, ctotal, fv0, fv1);
        int ok0 = 1, ok1 = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++) {
            if (fv0[i] > 0 && cl_u0[i] == 0) ok0 = 0;
            if (fv1[i] > 0 && cl_u1[i] == 0) ok1 = 0;
        }
        if (ok0 && ok1) {
            int bu = measure_encode_bits_ctx(q_v, ctotal, hc_u0, cl_u0, hc_u1, cl_u1);
            if (bu <= bv) {
                shared_v = 1; t_v0 = t_u0; t_v1 = t_u1;
                memcpy(cl0, cl_u0, sizeof(cl_u0)); memcpy(hc0, hc_u0, sizeof(hc_u0));
                memcpy(cl1, cl_u1, sizeof(cl_u1)); memcpy(hc1, hc_u1, sizeof(hc_u1));
            }
        }
    } else {
        int bv;  /* best V cost in bits (incl. table header for custom), from pick_best_table */
        count_frequencies(q_v, ctotal, freq);
        t_v0 = pick_best_table(q_v, ctotal, freq, 2, cl0, hc0, &bv, chroma_420);
        int ok = 1;
        for (int i = 0; i < NUM_HUFF_SYMBOLS; i++)
            if (freq[i] > 0 && cl_u0[i] == 0) { ok = 0; break; }
        if (ok) {
            int bu = measure_encode_bits(q_v, ctotal, hc_u0, cl_u0);
            if (bu <= bv) {
                shared_v = 1; t_v0 = t_u0; memcpy(cl0, cl_u0, sizeof(cl_u0)); memcpy(hc0, hc_u0, sizeof(hc_u0));
            }
        }
    }
    if (t_v0 == NUM_DEF_TABLES && !shared_v) t_v_size += write_huffman_table(&bs, cl0, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx && t_v1 == NUM_DEF_T1 && !shared_v) t_v_size += write_huffman_table(&bs, cl1, NUM_HUFF_SYMBOLS);
    if (huf_extra_ctx) huffman_encode_ctx(&bs, q_v, ctotal, hc0, cl0, hc1, cl1);
    else               huffman_encode_runval(&bs, q_v, ctotal, hc0, cl0);

    bitstream_flush(&bs);
    int stream_len = bitstream_bytes(&bs);

    int total_sz = hdr_sz + stream_len;
    uint8_t flags = 0;
    if (shared_v)       flags |= 1 << 5;
    if (huf_extra_ctx)  flags |= 1 << 6;
    flags |= ((t_v0 >> 2) & 1) << 7;  /* t0_v 3rd bit in flags */
    int pos = write_wtp_header(out, 0, flags, w, h, quality, chroma_420, has_alpha);
    /* tables byte: t0_y (3b), t0_u (3b), t0_v bits 0-1 (2b), t0_v bit 2 in flags */
    out[pos++] = (uint8_t)((t_y0 & 7) | ((t_u0 & 7) << 3) | ((t_v0 & 3) << 6));
    if (huf_extra_ctx)
        out[pos++] = (uint8_t)((t_y1 & 3) | ((t_u1 & 7) << 2) | ((t_v1 & 7) << 5));

    unsigned char *shrunk = (unsigned char*)realloc(out, total_sz); if (shrunk) out = shrunk;
    if (out_size) *out_size = total_sz;
    if (info) {
        info->ebcot = 0; info->encoded_bytes = total_sz; info->result_q = quality;
        info->huffman_y_size = t_y_size; info->huffman_u_size = t_u_size; info->huffman_v_size = t_v_size;
        info->huffman_y_table = t_y0; info->huffman_u_table = t_u0; info->huffman_v_table = t_v0;
    }
    return out;
}

/* Auto-find quality for target size via binary search. */
static unsigned char *find_quality_for_target(const float *y_w, const float *u_w, const float *v_w, const float *a_w, wtpc_enc_info *info, int w, int h, int cw, int ch, int target_bytes, int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha) {
    int total = w * h, ctotal = cw * ch;
    int16_t *q_y = malloc(total * sizeof(int16_t));
    int16_t *q_u = malloc(ctotal * sizeof(int16_t));
    int16_t *q_v = malloc(ctotal * sizeof(int16_t));
    int16_t *q_a = has_alpha ? malloc(total * sizeof(int16_t)) : NULL;
    if (!q_y || !q_u || !q_v || (has_alpha && !q_a)) {
        free(q_y); free(q_u); free(q_v); free(q_a);
        return 0;
    }

    #define PROBE(qv, out_d, out_i, out_sz) do { \
        quantize_coeffs(y_w, q_y, w, h, compute_base(qv), g_quant_y); \
        quantize_coeffs(u_w, q_u, cw, ch, compute_base(qv), (chroma_420) ? g_quant_c420 : g_quant_c); \
        quantize_coeffs(v_w, q_v, cw, ch, compute_base(qv), (chroma_420) ? g_quant_c420 : g_quant_c); \
        if (has_alpha) quantize_coeffs(a_w, q_a, w, h, compute_base(qv), g_quant_y); \
        if (huffman_mode == 2) { \
            *(out_d) = ebcot_pack(q_y, q_u, q_v, q_a, out_i, w, h, cw, ch, qv, chroma_420, out_sz); \
        } else if (huffman_mode == 1) { \
            *(out_d) = huffman_pack(q_y, q_u, q_v, q_a, out_i, w, h, cw, ch, qv, chroma_420, huf_extra_ctx, out_sz); \
        } else { \
            int _es, _hs; wtpc_enc_info _ei={0}, _hi={0}; \
            unsigned char *_e = ebcot_pack(q_y, q_u, q_v, q_a, &_ei, w, h, cw, ch, qv, chroma_420, &_es); \
            if (!_e) goto exit_error; \
            unsigned char *_h = huffman_pack(q_y, q_u, q_v, q_a, &_hi, w, h, cw, ch, qv, chroma_420, huf_extra_ctx, &_hs); \
            if (!_h) { free(_e); goto exit_error; } \
            if (_e && (!_h || _es <= _hs)) { \
                free(_h); *(out_d)=_e; *(out_sz)=_es; \
                _ei.ebcot = 1; _ei.encoded_bytes = _es; *out_i = _ei; \
            } else { \
                free(_e); *(out_d)=_h; *(out_sz)=_hs; \
                _hi.encoded_bytes = _hs; *out_i = _hi; \
            } \
        } \
    } while(0)

    /* First-step estimate for the pseudo-linear q scale of compute_base().       */
    /* size(q) is roughly a power law, but compute_base() is piecewise-linear     */
    /* (kinks at q=30 and q=80), so a single q ~ C*size^B misfits the tails by    */
    /* 20-400%. A 3-segment power-law fit (calibrated on the ~2800-image dataset, */
    /* mean_q per target) lands within ~5% of optimum everywhere,                 */
    /* which cuts the secant search to ~2-4 probes. Size scales sub-linearly with */
    /* pixel count (~npix^0.35), so we normalize the target to 256x256 first.     */
    /* Segments: [<=8K] shared; [8K..16K] and [>16K] differ per chroma mode       */
    /* (4:2:0 needs a lower q at high rates since chroma is already halved).      */
    #define Q_REF_NPIX  65536.0f   /* 256x256 calibration resolution */
    float npix = (float)(w * h);
    float target_n = (float)target_bytes * powf(Q_REF_NPIX / npix, 0.35f);
    if (target_n < 1.0f) target_n = 1.0f;
    float fit_c, fit_b;
    if (target_n <= 8000.0f) {
        fit_c = 6240.0f;      fit_b = -0.521f;   /* shared low segment */
    } else if (target_n <= 16000.0f) {
        if (chroma_420) { fit_c = 4.32e6f;  fit_b = -1.252f; }
        else            { fit_c = 2.57e6f;  fit_b = -1.193f; }
    } else {
        if (chroma_420) { fit_c = 7.5e12f;  fit_b = -2.735f; }
        else            { fit_c = 3.8e11f;  fit_b = -2.422f; }
    }
    int q = (int)(fit_c * powf(target_n, fit_b) + 0.5f);
    if (q < 1) q = 1;
    if (q > MAX_QUALITY) q = MAX_QUALITY;
    int steps = 0, best_q = q, best_dist = 99999999;
    unsigned char *best_data = NULL;
    wtpc_enc_info best_info = {0};
    int sizes[MAX_QUALITY + 1];
    memset(&sizes[0], 0, sizeof(sizes));

    /* Helper: probe quality, update best if improved. Always sets *out_sz. */
#ifdef WTPC_RC_ONLY_LESS_THAN_TARGET
    /* Only undershoot (size <= target). During tuning this avoids overshoot     */
    /* cheating the metrics; at runtime it guarantees the output fits the budget. */
    #define TRY(tq, out_sz) do { \
        int _sz; unsigned char *_d; wtpc_enc_info _ti; steps++; \
        PROBE(tq, &_d, &_ti, &_sz); \
        *(out_sz) = _sz; \
        if (_sz < 0) { free(_d); sizes[tq] = 0; break; } \
        sizes[tq] = _sz; \
        if (_sz > target_bytes) { free(_d); break; } \
        int _dist = target_bytes - _sz; \
        if (_dist < best_dist || (_dist == best_dist && tq < best_q)) { \
            best_dist = _dist; best_q = tq; \
            free(best_data); \
            best_data = _d; best_info = _ti; best_info.result_q = tq; \
        } else { free(_d); } \
    } while(0)
#else
    #define TRY(tq, out_sz) do { \
        int _sz; unsigned char *_d; wtpc_enc_info _ti; steps++; \
        PROBE(tq, &_d, &_ti, &_sz); \
        *(out_sz) = _sz; \
        if (_sz < 0) { free(_d); sizes[tq] = 0; break; } \
        sizes[tq] = _sz; \
        int _dist = abs(_sz - target_bytes); \
        if (_dist < best_dist || (_dist == best_dist && tq < best_q)) { \
            best_dist = _dist; best_q = tq; \
            free(best_data); \
            best_data = _d; best_info = _ti; best_info.result_q = tq; \
        } else { free(_d); } \
    } while(0)
#endif

    int sz;
    TRY(q, &sz);
    int first_q = q, first_sz = sz;
    int lo = 1, hi = MAX_QUALITY;
    if (first_sz > target_bytes) lo = first_q; else hi = first_q;

    /* Secant interpolation in log-log space. size(q) is monotone decreasing */
    /* and roughly a power law, so interpolating log(q) linearly in log(size) */
    /* converges very fast (typically 1-3 probes) once we bracket the target. */
    int pq = first_q, psz = first_sz;  /* previous probe */

    for (int iter = 0; iter < 12; iter++) {
        if (hi - lo <= 1) break;
        int nq;
        if (psz > 0 && psz != sz && q != pq && sz > 0) {
            float lt = logf((float)target_bytes);
            float l1 = logf((float)psz), l2 = logf((float)sz);
            float q1 = logf((float)pq), q2 = logf((float)q);
            float lq = q1 + (q2 - q1) * (lt - l1) / (l2 - l1);
            nq = (int)(expf(lq) + 0.5f);
        } else {
            nq = (lo + hi) / 2;  /* fall back to bisection */
        }
        if (nq <= lo) nq = lo + 1;
        if (nq >= hi) nq = hi - 1;
        if (nq == q || nq == pq) nq = (lo + hi) / 2;  /* avoid stalling */
        if (nq <= lo || nq >= hi) break;

        pq = q; psz = sz;
        q = nq;
        TRY(q, &sz);
        if (sz < 0) break;
        if (sz > target_bytes) { lo = q; }
        else                   { hi = q; }
    }

    /* Polish: probe -+1 around best_q to catch tiny non-monotonicities. */
    /* size(q) is near-monotone (measured: consecutive-q tie-runs = 0 across */
    /* the 200 B..36 KB range), so the true optimum sits within 1 q of best_q. */
    if (best_data) {
        int center = best_q;
        for (int d = -1; d <= 1; d += 2) {
            int tq = center + d;
            if (tq < 1 || tq > MAX_QUALITY) continue;
#ifdef WTPC_RC_ONLY_LESS_THAN_TARGET
            if (sizes[tq] > target_bytes) continue;  /* overshoot: skip in undershoot mode */
            if (sizes[tq] > 0) {
                int cd = target_bytes - sizes[tq];
#else
            if (sizes[tq] > 0) {
                int cd = abs(sizes[tq] - target_bytes);
#endif
                /* skip if provably not an improvement */
                if (cd > best_dist) continue;
                if (cd == best_dist && tq >= best_q) continue;
            }
            TRY(tq, &sz);
            if (best_dist == 0) break;  /* if d=-1 and it`s already ideal - do not check +1 */
        }
    }
    #undef TRY
    #undef PROBE
    #undef Q_REF_NPIX

    if (best_data) {
        memcpy(info, &best_info, sizeof(*info));
        info->search_steps = steps;
        free(q_y); free(q_u); free(q_v); free(q_a);
        return best_data;
    }
exit_error:
    memset(info, 0, sizeof(*info));
    free(q_y); free(q_u); free(q_v); free(q_a);
    return NULL;
}

unsigned char *wtpc_encode_mem(const unsigned char *rgb, wtpc_enc_info *info, int w, int h, int target_bytes, int quality, int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha) {
    memset(info, 0, sizeof(*info));
    if (w < 0 || w > 65536 || h < 0 || h > 65536 || (!target_bytes && (quality < 1 || quality > MAX_QUALITY))) return 0;

    /* Pre-compute wavelet coefficients (quality-independent, shared by all paths) */
    int total = w * h;
    int cw = w, ch = h;
    if (chroma_420) { cw = (w+1)/2; ch = (h+1)/2; }
    int ctotal = cw * ch;
    int stride = has_alpha ? 4 : 3;

    float *y_w = malloc(total * sizeof(float));
    float *u_full = malloc(total * sizeof(float));
    float *v_full = malloc(total * sizeof(float));
    float *a_w = has_alpha ? malloc(total * sizeof(float)) : NULL;
    if (!y_w || !u_full || !v_full || (has_alpha && !a_w)) { free(y_w); free(u_full); free(v_full); free(a_w); return NULL; }
    for (int i = 0; i < total; ++i) {
        rgb_to_yuv(rgb[i*stride], rgb[i*stride+1], rgb[i*stride+2], &y_w[i], &u_full[i], &v_full[i]);
        if (has_alpha) a_w[i] = (float)rgb[i*stride+3] - 128.0f;
    }

    float *u_w, *v_w;
    if (chroma_420) {
        u_w = malloc(ctotal * sizeof(float));
        v_w = malloc(ctotal * sizeof(float));
        chroma_down_420(u_full, w, h, u_w);
        chroma_down_420(v_full, w, h, v_w);
        free(u_full); free(v_full);
    } else { u_w = u_full; v_w = v_full; }

    cdf97_forward_2d(y_w, w, h);
    cdf97_forward_2d(u_w, cw, ch);
    cdf97_forward_2d(v_w, cw, ch);
    if (has_alpha) cdf97_forward_2d(a_w, w, h);

#ifdef DEBUG_WAVELET
    if (!target_bytes) {
        char path[128];
        snprintf(path, sizeof(path), "wavelet_y_before_q%d.png", quality);
        save_wavelet_png(y_w, w, h, path);
    }
#endif

    if (target_bytes) {
        unsigned char *out = find_quality_for_target(y_w, u_w, v_w, a_w, info, w, h, cw, ch, target_bytes, chroma_420, huffman_mode, huf_extra_ctx, has_alpha);
        free(y_w); free(u_w); free(v_w); free(a_w);
        return out;
    }

    /* Quantize + entropy code */
    int16_t *q_y = malloc(total * sizeof(int16_t));
    int16_t *q_u = malloc(ctotal * sizeof(int16_t));
    int16_t *q_v = malloc(ctotal * sizeof(int16_t));
    int16_t *q_a = has_alpha ? malloc(total * sizeof(int16_t)) : NULL;
    if (!q_y || !q_u || !q_v || (has_alpha && !q_a)) {
        free(y_w); free(u_w); free(v_w); free(a_w); free(q_y); free(q_u); free(q_v); free(q_a);
        return NULL;
    }
    quantize_coeffs(y_w, q_y, w, h, compute_base(quality), g_quant_y);
    quantize_coeffs(u_w, q_u, cw, ch, compute_base(quality), chroma_420 ? g_quant_c420 : g_quant_c);
    quantize_coeffs(v_w, q_v, cw, ch, compute_base(quality), chroma_420 ? g_quant_c420 : g_quant_c);
    if (has_alpha) quantize_coeffs(a_w, q_a, w, h, compute_base(quality), g_quant_y);

#ifdef DEBUG_WAVELET
    if (!target_bytes) {
        float *tmp = malloc(total * sizeof(float));
        if (tmp) {
            dequantize_channel(q_y, tmp, w, h, compute_base(quality), g_quant_y);
            char path[128];
            snprintf(path, sizeof(path), "wavelet_y_after_q%d.png", quality);
            save_wavelet_png(tmp, w, h, path);
            free(tmp);
        }
    }
#endif

    free(y_w); free(u_w); free(v_w); free(a_w);

    if (huffman_mode == 1) {
        unsigned char *out = huffman_pack(q_y, q_u, q_v, q_a, info, w, h, cw, ch, quality, chroma_420, huf_extra_ctx, 0);
        free(q_y); free(q_u); free(q_v); free(q_a);
        return out;
    }
    if (huffman_mode == 2) {
        unsigned char *out = ebcot_pack(q_y, q_u, q_v, q_a, info, w, h, cw, ch, quality, chroma_420, 0);
        free(q_y); free(q_u); free(q_v); free(q_a);
        return out;
    }
    /* Best mode: try both entropy coders on the same quantized data, pick smaller */
    int eb_sz, hf_sz;
    unsigned char *eb = ebcot_pack(q_y, q_u, q_v, q_a, info, w, h, cw, ch, quality, chroma_420, &eb_sz);
    unsigned char *hf = huffman_pack(q_y, q_u, q_v, q_a, info, w, h, cw, ch, quality, chroma_420, huf_extra_ctx, &hf_sz);
    free(q_y); free(q_u); free(q_v); free(q_a);

    if (eb && (!hf || eb_sz <= hf_sz)) { free(hf); memset(info, 0, sizeof(*info)); info->ebcot = 1; info->encoded_bytes = eb_sz; info->result_q = quality; return eb; }
    free(eb);
    info->encoded_bytes = hf_sz;
    return hf;
}

#ifndef WTPC_NO_STDIO
int wtpc_encode_file(const char *out_path, const unsigned char *rgb, wtpc_enc_info *info, int w, int h, int target_bytes, int quality, int chroma_420, int huffman_mode, int huf_extra_ctx, int has_alpha) {
    unsigned char *data = wtpc_encode_mem(rgb, info, w, h, target_bytes, quality, chroma_420, huffman_mode, huf_extra_ctx, has_alpha);
    if (!data) return -1;
    FILE *f = fopen(out_path, "wb");
    if (!f) { free(data); return -1; }
    fwrite(data, 1, info->encoded_bytes, f);
    fclose(f);
    free(data);
    return 0;
}
#endif  /* WTPC_NO_STDIO */

/* Returns malloc'd RGB buffer (w*h*3 bytes). Sets *w, *h, *out_quality. */
/* Returns NULL on error. */
unsigned char *wtpc_decode_mem(const unsigned char *data, int data_len, int *w, int *h, int *out_quality, int *out_comp) {
    if (data_len < (4 + 2 + 1) || strncmp((const char*)data, "WTP", 3) != 0) return NULL;

    int pos = 4, quality;
    uint8_t flags = data[3];
    *w = (int)data[pos] + 1; pos += 1;  /* stored as w-1 */
    *h = (int)data[pos] + 1; pos += 1;  /* stored as h-1 */
    int is_420 = (flags >> 0) & 1;
    int is_ebcot = (flags >> 1) & 1;
    int q_hi = (flags >> 2) & 1;
    int wh_hi = (flags >> 3) & 1;
    int alpha = (flags >> 4) & 1;
    int huf_shared_v_tbl = (flags >> 5) & 1;
    int huf_extra_ctx = (flags >> 6) & 1;  /* huffman uses 2 tables */
    int comp = alpha ? 4 : 3;
    if (out_comp) *out_comp = comp;

    if (!q_hi) { quality = data[pos++]; }
    else { uint16_t q16 = get_u16(data + pos); pos += 2; quality = q16; }
    if (out_quality) *out_quality = quality;
    /* Extended dimensions: hi bytes of (w-1) and (h-1) */
    if (wh_hi) { *w = ((*w - 1) | ((uint32_t)data[pos] << 8)) + 1; pos++; *h = ((*h - 1) | ((uint32_t)data[pos] << 8)) + 1; pos++; }

    if (is_ebcot)
        return ebcot_decode_mem(data, data_len, pos, *w, *h, quality, is_420, alpha);

    int tables = data[pos++];
    int t0_y = tables & HUFF_TBL_MASK;
    int t0_u = (tables >> 3) & HUFF_TBL_MASK;
    int t0_v = ((tables >> 6) & HUFF_TBL_MASK) | (((flags >> 7) & 1) << 2);  /* steal last bit from flags */
    /* Context Huffman: extra table selectors byte (Huffman mode only, after quality) */
    int t1_y = 0, t1_u = 0, t1_v = 0;
    if (huf_extra_ctx) {
        int extra_tables = data[pos++];
        t1_y = extra_tables & 3; t1_u = (extra_tables >> 2) & 7; t1_v = (extra_tables >> 5) & 7;
    }

    /* Huffman decode path - context-based: 2 tables/channel, prev-cat <=2 vs >2 */
    int total = (*w) * (*h);
    int cw = is_420 ? ((*w)+1)/2 : (*w), ch = is_420 ? ((*h)+1)/2 : (*h), ctotal = cw * ch;
    int16_t *q_y = (int16_t*)calloc(total, sizeof(int16_t));
    int16_t *q_u = (int16_t*)calloc(ctotal, sizeof(int16_t));
    int16_t *q_v = (int16_t*)calloc(ctotal, sizeof(int16_t));

    Bitstream bs;
    bitstream_init(&bs, data + pos, data_len - pos);
    if (huf_extra_ctx)
        t1_y |= get_bits(&bs, 1) << 2;
    /* Helper: read one table (selector 0-6=def,7=custom); picks 444 or 420 set by is_420 */
    #define RD_TBL(sel, ch, cl_out, hc_out, tbl444, tbl420) do { \
        const uint8_t (*tbl)[3][NUM_HUFF_SYMBOLS] = (is_420) ? (tbl420) : (tbl444); \
        if((sel) < NUM_DEF_TABLES) { \
            for(int _i=0;_i<NUM_HUFF_SYMBOLS;_i++) cl_out[_i]=tbl[sel][ch][_i]; \
        } else read_huffman_table(&bs, cl_out, NUM_HUFF_SYMBOLS); \
        generate_canonical_codes(cl_out, NUM_HUFF_SYMBOLS, hc_out); \
    } while(0)
    /* Y -> Alpha (alpha reuses Y's codes immediately after Y) */
    int cl[NUM_HUFF_SYMBOLS]; uint32_t hc[NUM_HUFF_SYMBOLS];
    int cl1[NUM_HUFF_SYMBOLS]; uint32_t hc1[NUM_HUFF_SYMBOLS];
    if (huf_extra_ctx) {
        RD_TBL(t0_y, 0, cl, hc, def_tables_t0, def_tables_t0_420);
        RD_TBL(t1_y, 0, cl1, hc1, def_tables_t1, def_tables_t1_420);
        huffman_decode_ctx(&bs, q_y, total, cl, hc, cl1, hc1);
    } else {
        RD_TBL(t0_y, 0, cl, hc, def_tables_single, def_tables_single_420);
        huffman_decode_channel(&bs, q_y, total, cl, hc);
    }

    /* Alpha: reuse Y's codes (still in cl/hc/cl1/hc1) */
    int16_t *q_a = NULL;
    if (alpha) {
        q_a = (int16_t*)calloc(total, sizeof(int16_t));
        if (!q_a) { free(q_y); free(q_u); free(q_v); return NULL; }
        if (huf_extra_ctx)
            huffman_decode_ctx(&bs, q_a, total, cl, hc, cl1, hc1);
        else
            huffman_decode_channel(&bs, q_a, total, cl, hc);
    }

    /* U (overwrites cl/hc/cl1/hc1 - Y's codes no longer needed) */
    int cl_u1[NUM_HUFF_SYMBOLS]; uint32_t hc_u1[NUM_HUFF_SYMBOLS];
    if (huf_extra_ctx) {
        RD_TBL(t0_u, 1, cl, hc, def_tables_t0, def_tables_t0_420);
        RD_TBL(t1_u, 1, cl_u1, hc_u1, def_tables_t1, def_tables_t1_420);
        huffman_decode_ctx(&bs, q_u, ctotal, cl, hc, cl_u1, hc_u1);
    } else {
        RD_TBL(t0_u, 1, cl, hc, def_tables_single, def_tables_single_420);
        huffman_decode_channel(&bs, q_u, ctotal, cl, hc);
    }
    /* V: reuse U's tables if huf_shared_v_tbl flag set */
    if (huf_shared_v_tbl) {
        if (huf_extra_ctx)
            huffman_decode_ctx(&bs, q_v, ctotal, cl, hc, cl_u1, hc_u1);
        else
            huffman_decode_channel(&bs, q_v, ctotal, cl, hc);
    } else {
        if (huf_extra_ctx) {
            RD_TBL(t0_v, 2, cl, hc, def_tables_t0, def_tables_t0_420);
            int cl1[NUM_HUFF_SYMBOLS]; uint32_t hc1[NUM_HUFF_SYMBOLS];
            RD_TBL(t1_v, 2, cl1, hc1, def_tables_t1, def_tables_t1_420);
            huffman_decode_ctx(&bs, q_v, ctotal, cl, hc, cl1, hc1);
        } else {
            RD_TBL(t0_v, 2, cl, hc, def_tables_single, def_tables_single_420);
            huffman_decode_channel(&bs, q_v, ctotal, cl, hc);
        }
    }
    #undef RD_TBL

    /* --- Dequantize + inverse wavelet --- */
    float *y_f = (float*)malloc(total * sizeof(float));
    float *u_s = (float*)malloc(ctotal * sizeof(float));
    float *v_s = (float*)malloc(ctotal * sizeof(float));
    float *a_f = alpha ? (float*)malloc(total * sizeof(float)) : NULL;
    if (!y_f || !u_s || !v_s || (alpha && !a_f)) {
        free(q_y); free(q_u); free(q_v); free(q_a);
        free(y_f); free(u_s); free(v_s); free(a_f);
        return NULL;
    }

    dequantize_channel(q_y, y_f, *w, *h, compute_base(quality), g_quant_y);
    dequantize_channel(q_u, u_s, cw, ch, compute_base(quality), is_420 ? g_quant_c420 : g_quant_c);
    dequantize_channel(q_v, v_s, cw, ch, compute_base(quality), is_420 ? g_quant_c420 : g_quant_c);
    if (alpha) dequantize_channel(q_a, a_f, *w, *h, compute_base(quality), g_quant_y);

    cdf97_inverse_2d(y_f, *w, *h);
    cdf97_inverse_2d(u_s, cw, ch);
    cdf97_inverse_2d(v_s, cw, ch);
    if (alpha) cdf97_inverse_2d(a_f, *w, *h);

    /* Upsample chroma if 4:2:0 */
    float *u_f, *v_f;
    if (is_420) {
        u_f = malloc(total*sizeof(float)); v_f = malloc(total*sizeof(float));
        chroma_up_420(u_s, cw, ch, u_f, *w, *h);
        chroma_up_420(v_s, cw, ch, v_f, *w, *h);
    } else { u_f = u_s; v_f = v_s; }

    uint8_t *out_img = (uint8_t*)malloc(total * comp);
    if (!out_img) { free(q_y); free(q_u); free(q_v); free(q_a); free(y_f); free(u_s); free(v_s); free(a_f); if (is_420) { free(u_f); free(v_f); } return NULL; }
    if (alpha) {
        for (int i = 0; i < total; ++i) {
            yuv_to_rgb(y_f[i], u_f[i], v_f[i],
                       &out_img[i * 4], &out_img[i * 4 + 1], &out_img[i * 4 + 2]);
            int av = (int)(a_f[i] + 128.5f); if (av < 0) av = 0; if (av > 255) av = 255;
            out_img[i * 4 + 3] = (uint8_t)av;
        }
    } else {
        for (int i = 0; i < total; ++i)
            yuv_to_rgb(y_f[i], u_f[i], v_f[i],
                       &out_img[i * 3], &out_img[i * 3 + 1], &out_img[i * 3 + 2]);
    }

    free(q_y); free(q_u); free(q_v); free(q_a);
    free(y_f); free(u_s); free(v_s); free(a_f);
    if (is_420) { free(u_f); free(v_f); }
    return out_img;
}

#ifndef WTPC_NO_STDIO
unsigned char *wtpc_decode_file(const char *in_path, int *w, int *h, int *out_quality, int *out_comp) {
    FILE *f = fopen(in_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char*)malloc(sz);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, sz, f) != (size_t)sz) { free(data); fclose(f); return NULL; }
    fclose(f);
    unsigned char *rgb = wtpc_decode_mem(data, (int)sz, w, h, out_quality, out_comp);
    free(data);
    return rgb;
}
#endif /* WTPC_NO_STDIO */

#endif /* WTPC_IMAGE_IMPLEMENTATION */
