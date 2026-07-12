#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <png.h>

#define WTPC_IMAGE_IMPLEMENTATION
#include "wtpc_image.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef WTPC_TUNE_PARAMS
#define TUNE_THREADS 7
#define MAX_WORSE_STEPS 3
#define MAX_WORSE_SSIM  0.5f
float g_dz_factor    = DZ_FACTOR;
static int g_tune_verbose = 0;
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* --- libpng-based PNG reader with ICC->sRGB via lcms2 --- */
#include <lcms2.h>
static uint8_t *read_png(const char *path, int *w, int *h, int *has_alpha) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    png_init_io(png, fp);
    png_read_info(png, info);
    *w = png_get_image_width(png, info);
    *h = png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    int bit_depth = png_get_bit_depth(png, info);
    /* Extract ICC profile for lcms2 conversion (before libpng frees it) */
    png_charp icc_name = NULL;
    png_bytep icc_ptr = NULL;
    png_uint_32 icc_len = 0;
    int has_icc = png_get_iCCP(png, info, &icc_name, NULL, &icc_ptr, &icc_len);
    int is_srgb = png_get_valid(png, info, PNG_INFO_sRGB);
    uint8_t *icc_copy = NULL;
    if (has_icc && !is_srgb) { icc_copy = malloc(icc_len); if (icc_copy) memcpy(icc_copy, icc_ptr, icc_len); }
    /* Normalize to 8-bit RGB/RGBA */
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    int alpha_flag = 0;
    if (color_type & PNG_COLOR_MASK_ALPHA) alpha_flag = 1;
    else if (png_get_valid(png, info, PNG_INFO_tRNS)) { png_set_tRNS_to_alpha(png); alpha_flag = 1; }
    else png_set_strip_alpha(png);
    png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    png_read_update_info(png, info);
    int rowbytes = png_get_rowbytes(png, info);
    uint8_t *data = malloc(*h * rowbytes);
    if (!data) { free(icc_copy); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    png_bytep *rows = malloc(*h * sizeof(png_bytep));
    if (!rows) { free(data); free(icc_copy); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    for (int y = 0; y < *h; y++) rows[y] = data + y * rowbytes;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    /* Apply ICC->sRGB conversion via lcms2 (skip if profile is corrupted) */
    if (icc_copy) {
        cmsHPROFILE in_prof = cmsOpenProfileFromMem(icc_copy, icc_len);
        if (in_prof) {
            cmsHPROFILE out_prof = cmsCreate_sRGBProfile();
            cmsHTRANSFORM xfrm = NULL;
            if (out_prof) {
                xfrm = cmsCreateTransform(in_prof,
                    alpha_flag ? TYPE_RGBA_8 : TYPE_RGB_8,
                    out_prof,
                    alpha_flag ? TYPE_RGBA_8 : TYPE_RGB_8,
                    INTENT_PERCEPTUAL, 0);
            }
            if (xfrm) { cmsDoTransform(xfrm, data, data, *w * *h); cmsDeleteTransform(xfrm); }
            if (out_prof) cmsCloseProfile(out_prof);
            cmsCloseProfile(in_prof);
        } else {
            fprintf(stderr, "read_png: bad ICC profile in %s, skipping color conversion\n", path);
        }
        free(icc_copy);
    }
    *has_alpha = alpha_flag;
    return data;
}

/* --- Generate Huffman tables from image dataset --- */
/* Single-pass: one quantization per quality level collects both */
/* single-table frequencies AND context-aware (prev-cat<=2 / prev-cat>2). */
/* Produces three table sets for 4:4:4 + three for 4:2:0: */
/*   def_tables_single, def_tables_t0, def_tables_t1 */
/*   def_tables_single_420, def_tables_t0_420, def_tables_t1_420 */
static void generate_tables(const char *dir_path) {
    int q_levels[NUM_DEF_TABLES] = {400, 200, 100, 50, 20, 8, 3};

    int64_t freq_single[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    int64_t freq_t0[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    int64_t freq_t1[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    int64_t freq_single_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    int64_t freq_t0_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    int64_t freq_t1_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS];
    memset(freq_single, 0, sizeof(freq_single));
    memset(freq_t0, 0, sizeof(freq_t0));
    memset(freq_t1, 0, sizeof(freq_t1));
    memset(freq_single_420, 0, sizeof(freq_single_420));
    memset(freq_t0_420, 0, sizeof(freq_t0_420));
    memset(freq_t1_420, 0, sizeof(freq_t1_420));
    int img_count = 0;

    DIR *d = opendir(dir_path);
    if (!d) { fprintf(stderr, "Cannot open: %s\n", dir_path); return; }
    int limit = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && (!limit || img_count < limit)) {
        const char *name = ent->d_name;
        const char *ext = strrchr(name, '.');
        if (!ext || (strcmp(ext, ".png") && strcmp(ext, ".jpg"))) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);
        int w, h, comp;
        uint8_t *rgb = stbi_load(path, &w, &h, &comp, 3);
        if (!rgb) continue;

        int total = w * h;
        float *y_w = malloc(total * sizeof(float));
        float *u_w = malloc(total * sizeof(float));
        float *v_w = malloc(total * sizeof(float));
        int16_t *q_y = malloc(total * sizeof(int16_t));
        int16_t *q_u = malloc(total * sizeof(int16_t));
        int16_t *q_v = malloc(total * sizeof(int16_t));
        if (!y_w || !u_w || !v_w || !q_y || !q_u || !q_v) {
            free(y_w); free(u_w); free(v_w); free(q_y); free(q_u); free(q_v);
            stbi_image_free(rgb); continue;
        }
        for (int i = 0; i < total; ++i)
            rgb_to_yuv(rgb[i*3], rgb[i*3+1], rgb[i*3+2], &y_w[i], &u_w[i], &v_w[i]);
        stbi_image_free(rgb);

        /* 4:2:0 path: downsample chroma BEFORE wavelet (u_w/v_w still in pixel domain) */
        int cw420 = (w+1)/2, ch420 = (h+1)/2, ctotal420 = cw420 * ch420;
        float *u_ds = malloc(ctotal420 * sizeof(float));
        float *v_ds = malloc(ctotal420 * sizeof(float));
        int16_t *q_u420 = malloc(ctotal420 * sizeof(int16_t));
        int16_t *q_v420 = malloc(ctotal420 * sizeof(int16_t));
        if (!u_ds || !v_ds || !q_u420 || !q_v420) {
            free(u_ds); free(v_ds); free(q_u420); free(q_v420);
            free(y_w); free(u_w); free(v_w); free(q_y); free(q_u); free(q_v);
            continue;
        }
        chroma_down_420(u_w, w, h, u_ds);
        chroma_down_420(v_w, w, h, v_ds);
        cdf97_forward_2d(u_ds, cw420, ch420);
        cdf97_forward_2d(v_ds, cw420, ch420);

        /* 4:4:4 path: forward wavelet on full-res U/V (after 4:2:0 downsampled from pixel values above) */
        cdf97_forward_2d(y_w, w, h);
        cdf97_forward_2d(u_w, w, h);
        cdf97_forward_2d(v_w, w, h);

        for (int lev = 0; lev < NUM_DEF_TABLES; lev++) {
            int q = q_levels[lev];
            float base = compute_base(q);

            /* --- 4:4:4: quantize + count --- */
            quantize_coeffs(y_w, q_y, w, h, base, g_quant_y);
            quantize_coeffs(u_w, q_u, w, h, base, g_quant_c);
            quantize_coeffs(v_w, q_v, w, h, base, g_quant_c);

            int fs[NUM_HUFF_SYMBOLS];
            count_frequencies(q_y, total, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][0][s] += fs[s];
            count_frequencies(q_u, total, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][1][s] += fs[s];
            count_frequencies(q_v, total, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][2][s] += fs[s];

            int f0[NUM_HUFF_SYMBOLS], f1[NUM_HUFF_SYMBOLS];
            count_frequencies_ctx(q_y, total, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][0][s] += f0[s]; freq_t1[lev][0][s] += f1[s]; }
            count_frequencies_ctx(q_u, total, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][1][s] += f0[s]; freq_t1[lev][1][s] += f1[s]; }
            count_frequencies_ctx(q_v, total, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][2][s] += f0[s]; freq_t1[lev][2][s] += f1[s]; }

            /* --- 4:2:0: quantize half-res U/V, Y already quantized from 4:4:4 --- */
            quantize_coeffs(u_ds, q_u420, cw420, ch420, base, g_quant_c420);
            quantize_coeffs(v_ds, q_v420, cw420, ch420, base, g_quant_c420);

            /* Y: recount from q_y (fs/f0/f1 were overwritten by U/V above), then accumulate */
            count_frequencies(q_y, total, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][0][s] += fs[s];
            count_frequencies(q_u420, ctotal420, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][1][s] += fs[s];
            count_frequencies(q_v420, ctotal420, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][2][s] += fs[s];

            count_frequencies_ctx(q_y, total, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][0][s] += f0[s]; freq_t1_420[lev][0][s] += f1[s]; }
            count_frequencies_ctx(q_u420, ctotal420, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][1][s] += f0[s]; freq_t1_420[lev][1][s] += f1[s]; }
            count_frequencies_ctx(q_v420, ctotal420, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][2][s] += f0[s]; freq_t1_420[lev][2][s] += f1[s]; }
        }
        free(y_w); free(u_w); free(v_w); free(q_y); free(q_u); free(q_v);
        free(u_ds); free(v_ds); free(q_u420); free(q_v420);
        img_count++;
        fprintf(stderr, "\r%d images", img_count); fflush(stderr);
    }
    closedir(d);
    fprintf(stderr, "\nTotal: %d images\n", img_count);
    if (img_count == 0) return;

    /* Helper: output one table level */
    void put(int64_t f[NUM_HUFF_SYMBOLS]) {
        int fi[NUM_HUFF_SYMBOLS];
        for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) {
            int64_t v = f[s];
            fi[s] = (int)(v > INT_MAX ? INT_MAX : v);
        }
        if (fi[EOB_SYMBOL] == 0) fi[EOB_SYMBOL] = 1;
        int cl[NUM_HUFF_SYMBOLS];
        build_huffman_codes(fi, NUM_HUFF_SYMBOLS, cl);
        printf("{");
        for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) {
            printf("%d", cl[s]);
            if (s < NUM_HUFF_SYMBOLS - 1) printf(",");
        }
        printf("}");
    }
    void put_set(int64_t freq[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS]) {
        for (int lev = 0; lev < NUM_DEF_TABLES; lev++) {
            printf(" {");
            for (int ch = 0; ch < 3; ch++) {
                int64_t f[NUM_HUFF_SYMBOLS];
                for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) f[s] = freq[lev][ch][s];
                put(f);
                if (ch < 2) printf(",");
            }
            printf("}");
            if (lev < NUM_DEF_TABLES - 1) printf(",\n"); else printf("\n");
        }
    }

    printf("// Auto-generated Huffman tables from %d images\n", img_count);
    printf("// Quality levels: {%d,%d,%d,%d,%d,%d,%d}\n",
        q_levels[0], q_levels[1], q_levels[2], q_levels[3], q_levels[4], q_levels[5], q_levels[6]);
    printf("\n");
    printf("// Single-table (huf_extra_ctx=0): all coefficients\n");
    printf("static const uint8_t def_tables_single[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_single);
    printf("};\n\n");
    printf("// t0 tables (prev-cat <= 2)\n");
    printf("static const uint8_t def_tables_t0[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t0);
    printf("};\n\n");
    printf("// t1 tables (prev-cat > 2)\n");
    printf("static const uint8_t def_tables_t1[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t1);
    printf("};\n\n");
    printf("// --- 4:2:0 chroma subsampling variants ---\n");
    printf("// Single-table (huf_extra_ctx=0): all coefficients\n");
    printf("static const uint8_t def_tables_single_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_single_420);
    printf("};\n\n");
    printf("// t0 tables (prev-cat <= 2)\n");
    printf("static const uint8_t def_tables_t0_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t0_420);
    printf("};\n\n");
    printf("// t1 tables (prev-cat > 2)\n");
    printf("static const uint8_t def_tables_t1_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t1_420);
    printf("};\n");
}

#ifdef WTPC_TUNE_PARAMS

enum {
    P_BAND0, P_BAND1, P_BAND2, P_BAND3, P_BAND4, P_BAND5, P_BAND6, P_BAND7,
    P_CBAND0, P_CBAND1, P_CBAND2, P_CBAND3, P_CBAND4, P_CBAND5, P_CBAND6, P_CBAND7,
    P_DZ,
    P420_BAND0, P420_BAND1, P420_BAND2, P420_BAND3, P420_BAND4, P420_BAND5, P420_BAND6, P420_BAND7,
    NPARAMS
};
#define NPARAMS_MAIN (P_DZ + 1)
#define NPARAMS_420  (NPARAMS - NPARAMS_MAIN)

typedef struct {
    const char *name;
    float delta;  /* step size */
    float win_size;  /* max +-deviation from center */
} ParamCfg;

static const ParamCfg param_cfg[NPARAMS_MAIN] = {
    /* Luma bands */
    {"b0(coarsest)", 0.01f, 0.05f},
    {"b1",           0.01f, 0.05f},
    {"b2",           0.01f, 0.05f},
    {"b3",           0.01f, 0.05f},
    {"b4",           0.01f, 0.05f},
    {"b5",           0.01f, 0.05f},
    {"b6",           0.01f, 0.10f},
    {"b7(finest)",   0.01f, 0.20f},
    /* Chroma 4:4:4 bands */
    {"cb0(coarsest)",0.01f, 0.6f},
    {"cb1",          0.01f, 0.6f},
    {"cb2",          0.01f, 0.6f},
    {"cb3",          0.01f, 0.6f},
    {"cb4",          0.01f, 0.6f},
    {"cb5",          0.01f, 0.6f},
    {"cb6",          0.01f, 0.7f},
    {"cb7(finest)",  0.01f, 1.0f},
    /* DZ */
    {"dz_factor",    0.01f, 0.02f},
};

static const ParamCfg param_cfg_420[NPARAMS_420] = {
    {"c420b0(coarsest)", 0.10f, 2.0f},
    {"c420b1",           0.10f, 2.0f},
    {"c420b2",           0.10f, 2.0f},
    {"c420b3",           0.10f, 2.0f},
    {"c420b4",           0.10f, 2.5f},
    {"c420b5",           0.10f, 2.5f},
    {"c420b6",           0.10f, 3.0f},
    {"c420b7(finest)",   0.10f, 4.0f},
};

static float g_params[NPARAMS];

static void apply_params(void) {
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y[b]    = g_params[P_BAND0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c[b]    = g_params[P_CBAND0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420[b] = g_params[P420_BAND0 + b];
    g_dz_factor = g_params[P_DZ];
}

static void print_all_params(int chroma) {
    printf("# dz:   %.2ff\n", g_dz_factor);
    if (!chroma) {
        printf("static WTPC_TABLES_CONST float g_quant_y[MAX_BANDS]    = {"); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? ", " : "", g_quant_y[b]); printf("};\n");
        printf("static WTPC_TABLES_CONST float g_quant_c[MAX_BANDS]    = {"); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? ", " : "", g_quant_c[b]); printf("};\n");
    } else {
        printf("static WTPC_TABLES_CONST float g_quant_c420[MAX_BANDS] = {"); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? ", " : "", g_quant_c420[b]); printf("};\n");
    }
    fflush(stdout);
}

typedef struct {
    uint8_t *rgb;
    int w, h;
    int has_alpha;
} PreloadedImg;

/* Per-target aggregate: one instance per (thread, target) pair, */
/* merged across threads after workers join.                      */
typedef struct {
    double ssim_sum;   /* sum of ssimulacra2 scores */
    int    count;      /* number of valid measurements */
    int    max_dev;    /* max absolute deviation (bytes) */
    double dev_sum;    /* sum of absolute deviations (bytes) */
} PerTargetStats;

typedef struct {
    int tid;
    int chroma_mode;   /* 0 = 4:4:4 only, 1 = 4:2:0 only */
    /* shared work counter */
    int *next_idx;
    pthread_mutex_t *mutex;
    pthread_mutex_t *print_mutex;
    /* read-only input */
    const char *dir_path;
    char **names;
    PreloadedImg *images;  /* pre-decoded RGB (owned by tune_grid) */
    int nimg;
    int *targets;
    int ntargets;
    int ntotal;        /* nimg * ntargets */
    /* per-target stats (allocated per thread, [ntargets]) */
    PerTargetStats *stats;
} ThreadCtx;

static void *tune_worker(void *arg) {
    ThreadCtx *ctx = (ThreadCtx*)arg;

    stbi_write_png_compression_level = 1;  /* faster png write */
    while (1) {
        pthread_mutex_lock(ctx->mutex);
        int idx = (*ctx->next_idx)++;
        pthread_mutex_unlock(ctx->mutex);
        if (idx >= ctx->ntotal) break;

        /* Single chroma mode: idx -> (t, i) */
        int t    = idx / ctx->nimg;
        int i    = idx % ctx->nimg;
        int tidx = t;

        PreloadedImg *pimg = &ctx->images[i];
        int w = pimg->w, h = pimg->h;
        uint8_t *img = pimg->rgb;

        wtpc_enc_info info;
        unsigned char *enc = wtpc_encode_mem(img, &info, w, h, ctx->targets[t], 0, ctx->chroma_mode, 2, 0, pimg->has_alpha);
        if (!enc) continue;

        int dw, dh, dq, dcomp;
        unsigned char *dec = wtpc_decode_mem(enc, info.encoded_bytes, &dw, &dh, &dq, &dcomp);
        free(enc);
        if (!dec) continue;

        char tmp_png[256];
        snprintf(tmp_png, sizeof(tmp_png), "/tmp/wtpc_tune_%d_%d_%d_%d.png", getpid(), ctx->tid, i, ctx->targets[t]);
        stbi_write_png(tmp_png, dw, dh, dcomp, dec, dw*dcomp);
        free(dec);

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", ctx->dir_path, ctx->names[i]);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "ssimulacra2 \"%s\" \"%s\" 2>/dev/null", path, tmp_png);
        FILE *fp = popen(cmd, "r");
        float score = 0;
        int ss_ok = 0;
        if (fp) { ss_ok = (fscanf(fp, "%f", &score) == 1); pclose(fp); }
        remove(tmp_png);

        if (!ss_ok) {
            if (g_tune_verbose) {
                pthread_mutex_lock(ctx->print_mutex);
                fprintf(stderr, "[th%d t=%d %s] %s: SSIMULACRA2 FAILED (bad ICC? skipping)\n", ctx->tid, ctx->targets[t], ctx->chroma_mode ? "420" : "444", ctx->names[i]);
                pthread_mutex_unlock(ctx->print_mutex);
            }
            continue;
        }

        PerTargetStats *st = &ctx->stats[tidx];
        st->ssim_sum += score;
        st->count++;
        int dev = abs(info.encoded_bytes - ctx->targets[t]);
        if (dev > 0 && info.encoded_bytes < ctx->targets[t] && info.result_q <= 1) dev = 0;
        st->dev_sum += dev;
        if (dev > st->max_dev) st->max_dev = dev;

        if (g_tune_verbose || (t == (ctx->ntargets - 1) && score < 69.0f && strcmp(ctx->names[i], "n01807496_partridge_256.png"))) {
            pthread_mutex_lock(ctx->print_mutex);
            fprintf(stderr, "[th%d t=%d] %s: ssim2=%11.6f n=%3d q=%3d dev=%3d avg_s=%11.6f mean_d=%.1f max_d=%3d (%.1f%%) - %s\n",
                ctx->tid, ctx->targets[t], ctx->chroma_mode ? "420" : "444", score, st->count, info.result_q, dev,
                st->count > 0 ? st->ssim_sum / st->count : 0.0, st->count > 0 ? st->dev_sum / st->count : 0.0,
                st->max_dev, (st->max_dev*100.f/ctx->targets[t]), ctx->names[i]);
            pthread_mutex_unlock(ctx->print_mutex);
        }
    }
    return NULL;
}

/* --- Automated grid-search: sweeps one param at a time, keeps best, continues --- */
static void tune_grid(const char *dir_path, const ParamCfg *cfg, int nparams, int base_idx, int chroma_mode, int start_param) {
    if (system("which ssimulacra2 >/dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: ssimulacra2 not found in PATH\n");
        return;
    }

    int targets[] = {1000, 2000, 4000, 8000, 16000, 32000, 36000};
    int ntargets = sizeof(targets) / sizeof(targets[0]);

    /* Init g_params from current g_quant tables (start from previous run's values) */
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_BAND0 + b] = g_quant_y[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_CBAND0 + b] = g_quant_c[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P420_BAND0 + b] = g_quant_c420[b];
    g_params[P_DZ] = g_dz_factor;
    apply_params();

    /* Collect image list */
    DIR *d = opendir(dir_path);
    if (!d) { fprintf(stderr, "Cannot open: %s\n", dir_path); return; }
    char **names = NULL; int nimg = 0, ncap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || (strcmp(ext, ".png") && strcmp(ext, ".jpg"))) continue;
        if (nimg >= ncap) { ncap = ncap ? ncap*2 : 256; names = realloc(names, ncap * sizeof(char*)); }
        names[nimg++] = strdup(ent->d_name);
    }
    closedir(d);
    if (nimg == 0) { fprintf(stderr, "No images found\n"); return; }

    /* Preload all images into RAM (avoids re-decoding PNG every encode) */
    PreloadedImg *images = malloc(nimg * sizeof(PreloadedImg));
    if (!images) { fprintf(stderr, "OOM preloading images\n"); return; }
    int64_t total_rgb = 0;
    for (int i = 0; i < nimg; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, names[i]);
        const char *ext = strrchr(names[i], '.');
        if (ext && strcmp(ext, ".png") == 0) {
            images[i].rgb = read_png(path, &images[i].w, &images[i].h, &images[i].has_alpha);
        } else {
            int comp;
            images[i].rgb = stbi_load(path, &images[i].w, &images[i].h, &comp, 0);
            images[i].has_alpha = (comp == 4);
        }
        if (!images[i].rgb) {
            fprintf(stderr, "Failed to load: %s\n", names[i]);
            images[i].w = images[i].h = 0;
            images[i].has_alpha = 0;
        } else {
            int comp = images[i].has_alpha ? 4 : 3;
            total_rgb += (int64_t)images[i].w * images[i].h * comp;
        }
    }
    fprintf(stderr, "Preloaded %d images (%.1f MB)\n", nimg, total_rgb / (1024.0 * 1024.0));

    /* Count max total steps (center + both directions up to win_size) */
    int total = 0, total_steps = 0;
    for (int p = 0; p < nparams; p++) {
        int max_d = (int)(cfg[p].win_size / cfg[p].delta + 0.5f);
        int ns = 1 + 2 * max_d;  /* center +-1..max_d */
        total += ns * nimg * ntargets;
        total_steps += ns;
    }
    fprintf(stderr, "Grid: %d images x %d targets x %d params = %d encodes (max)\n\n", nimg, ntargets, nparams, total);

    printf("# param,value,avg_ssim2\n");

    /* Sweep each parameter: start from center, expand +-delta outward */
    int step_num = 0;
    if (start_param > 0) {
        for (int pp = 0; pp < start_param; pp++) {
            int max_d = (int)(cfg[pp].win_size / cfg[pp].delta + 0.5f);
            step_num += 1 + 2 * max_d;
        }
    }
    double t_grid_start = now_ms();

    /* Macro: evaluate one value of param p, set *ssim_out = avg ssim2 */
    #define TUNE_EVAL(val, ssim_out, label) do { \
        g_params[base_idx + p] = (val); \
        apply_params(); \
        double _t_step = now_ms(); \
        int _ntotal = nimg * ntargets; \
        int _nth = _ntotal < TUNE_THREADS ? _ntotal : TUNE_THREADS; \
        int _next_idx = 0; \
        pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER; \
        pthread_mutex_t _pmutex = PTHREAD_MUTEX_INITIALIZER; \
        PerTargetStats *_astats = calloc(_nth * ntargets, sizeof(PerTargetStats)); \
        ThreadCtx _ctx[TUNE_THREADS]; \
        pthread_t _th[TUNE_THREADS]; \
        for (int _tid = 0; _tid < _nth; _tid++) { \
            _ctx[_tid].tid         = _tid; \
            _ctx[_tid].chroma_mode = chroma_mode; \
            _ctx[_tid].next_idx    = &_next_idx; \
            _ctx[_tid].mutex       = &_mutex; \
            _ctx[_tid].print_mutex = &_pmutex; \
            _ctx[_tid].dir_path    = dir_path; \
            _ctx[_tid].names       = names; \
            _ctx[_tid].images      = images; \
            _ctx[_tid].nimg        = nimg; \
            _ctx[_tid].targets     = targets; \
            _ctx[_tid].ntargets    = ntargets; \
            _ctx[_tid].ntotal      = _ntotal; \
            _ctx[_tid].stats       = _astats + _tid * ntargets; \
            memset(_ctx[_tid].stats, 0, ntargets * sizeof(PerTargetStats)); \
            pthread_create(&_th[_tid], NULL, tune_worker, &_ctx[_tid]); \
        } \
        for (int _tid = 0; _tid < _nth; _tid++) pthread_join(_th[_tid], NULL); \
        pthread_mutex_destroy(&_mutex); \
        pthread_mutex_destroy(&_pmutex); \
        /* Aggregate across threads, print per-target breakdown */ \
        double _sum_ssim2 = 0; int _count = 0; \
        int _overall_max_dev = 0; \
        double _overall_max_dev_pct = 0; \
        double _overall_mean_dev_sum = 0; \
        double _overall_mean_pct_sum = 0; \
        for (int _tidx = 0; _tidx < ntargets; _tidx++) { \
            double _ts = 0, _td = 0; int _tn = 0, _tm = 0; \
            for (int _tid = 0; _tid < _nth; _tid++) { \
                PerTargetStats *_st = &_astats[_tid * ntargets + _tidx]; \
                _ts += _st->ssim_sum; _tn += _st->count; \
                _td += _st->dev_sum; \
                if (_st->max_dev > _tm) _tm = _st->max_dev; \
            } \
            if (_tn > 0) { \
                double _as = _ts / _tn; \
                double _ad = _td / _tn; \
                _sum_ssim2 += _as; _count++; \
                double _dp = _ad * 100.0 / targets[_tidx]; \
                double _mp = _tm * 100.0 / targets[_tidx]; \
                if (_tm > _overall_max_dev) { _overall_max_dev = _tm; _overall_max_dev_pct = _mp; } \
                _overall_mean_dev_sum += _ad; \
                _overall_mean_pct_sum += _dp; \
                fprintf(stderr, "[t=%d n=%d ssim2=%-9.6f mean_d=%.1f (%.1f%%) max_d=%d (%.1f%%)]%s", targets[_tidx], _tn, _as, _ad, _dp, _tm, _mp, _tidx == (ntargets - 1) ? "\n" : " "); \
            } \
        } \
        free(_astats); \
        double _avg = _count > 0 ? _sum_ssim2 / _count : 0; \
        *(ssim_out) = _avg; \
        printf("%s, %.3f, %.6f\n", pcfg->name, (val), _avg); \
        fflush(stdout); \
        double _dt_s = (now_ms() - _t_step) * 0.001; \
        step_num++; \
        int _remain = total_steps - step_num; \
        double _gm_dev = _count > 0 ? _overall_mean_dev_sum / _count : 0; \
        double _gm_pct = _count > 0 ? _overall_mean_pct_sum / _count : 0; \
        fprintf(stderr, "  %-15s %s = %6.3f  ssim2=%-9.6f  (best: %.3f = %.6f)  mean_dev=%.1f (%.1f%%)  max_dev=%d (%.1f%%)\n", pcfg->name, label, (val), _avg, best_val, best_ssim, _gm_dev, _gm_pct, _overall_max_dev, _overall_max_dev_pct); \
        fprintf(stderr, "  step %d/%d (%d%%)  dt=%.1fs  eta=~%.0fm\n", step_num, total_steps, step_num*100/total_steps, _dt_s, _dt_s*_remain/60.0); \
    } while(0)

    for (int p = start_param; p < nparams; p++) {
        const ParamCfg *pcfg = &cfg[p];
        float center = g_params[base_idx + p];
        int max_d = (int)(pcfg->win_size / pcfg->delta + 0.5f);

        fprintf(stderr, "--- Tuning %-15s (center=%.3f delta=%.3f win=%.3f) ---\n", pcfg->name, center, pcfg->delta, pcfg->win_size);
        print_all_params(chroma_mode);

        float best_val = center, best_ssim, ssim;
        TUNE_EVAL(center, &best_ssim, " C");

        int pos_stop = 0, neg_stop = 0, pos_down = 0, neg_down = 0;
        float prev_pos = best_ssim, prev_neg = best_ssim;
        int skipped_pos = 0, skipped_neg = 0;

        for (int d = 1; d <= max_d && (!pos_stop || !neg_stop); d++) {
            if (!pos_stop) {
                TUNE_EVAL(center + d * pcfg->delta, &ssim, " +");
                if (ssim >= best_ssim) { best_ssim = ssim; best_val = center + d * pcfg->delta; pos_down = 0; }
                else {
                    pos_down++;
                    if (pos_down >= MAX_WORSE_STEPS || (prev_pos - ssim) > MAX_WORSE_SSIM) { pos_stop = 1; skipped_pos = max_d - d; fprintf(stderr, "  Positive search terminated.\n"); }
                }
                prev_pos = ssim;
            }
            if (!neg_stop) {
                float val = center - d * pcfg->delta;
                if (val <= 0.0f) { neg_stop = 1; skipped_neg = max_d - d + 1; }
                else {
                    TUNE_EVAL(val, &ssim, " -");
                    if (ssim > best_ssim) { best_ssim = ssim; best_val = val; neg_down = 0; }
                    else {
                        neg_down++;
                        if (neg_down >= MAX_WORSE_STEPS || (prev_neg - ssim) > MAX_WORSE_SSIM) { neg_stop = 1; skipped_neg = max_d - d; fprintf(stderr, "  Negative search terminated.\n"); }
                    }
                    prev_neg = ssim;
                }
            }
        }
        total_steps -= skipped_pos + skipped_neg;

        g_params[base_idx + p] = best_val;
        apply_params();

        fprintf(stderr, "\n>>> BEST %s = %.3f (ssim2 = %.2f)\n", pcfg->name, best_val, best_ssim);
        printf("# BEST_%s, %.3f, %.2f\n", pcfg->name, best_val, best_ssim);
        print_all_params(chroma_mode);
        fprintf(stderr, "\n");
    }
    #undef TUNE_EVAL

    fprintf(stderr, "\n=== Final best params ===\n");
    print_all_params(chroma_mode);
    fprintf(stderr, "Total time: %.1f min\n", (now_ms() - t_grid_start) * 0.001 / 60.0);

    for (int i = 0; i < nimg; i++) free(names[i]);
    free(names);
    for (int i = 0; i < nimg; i++) { if (images[i].rgb) stbi_image_free(images[i].rgb); }
    free(images);
}
#endif  /* WTPC_TUNE_PARAMS */

int main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int mode = -1;  /* 0=decode, 1=encode, 2=test */
    int quality = 20;
    int chroma_420 = 0;
    int huffman_mode = 2;  /* 0=auto, 1=huff, 2=ebcot. default to ebcot because it currently winning, no need try huffman and slower -b */
    int target_bytes = 0;  /* 0 means use -q directly */
    int huf_extra_ctx = 0;  /* 0=single table, faster, 1 - context-aware (2 Huffman tables) for better compression, slower */
#ifdef WTPC_TUNE_PARAMS
    int tune_start = 0;  /* -S: start grid from this param index (0=first) */
    int tune_420   = 0;  /* -420: tune chroma 4:2:0 params instead of main */
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) mode = 1;
        else if (strcmp(argv[i], "-d") == 0) mode = 0;
        else if (strcmp(argv[i], "-t") == 0) mode = 2;
        else if (strcmp(argv[i], "-q") == 0 && i+1 < argc) quality = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) target_bytes = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0) chroma_420 = 1;
        else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) { const char *m = argv[++i]; if (strcmp(m, "huffman") == 0) huffman_mode = 1; else if (strcmp(m, "ebcot") == 0) huffman_mode = 2; }
        else if (strcmp(argv[i], "-h") == 0 && i+1 < argc) huf_extra_ctx = atoi(argv[++i]);
        else if (strcmp(argv[i], "-G") == 0) mode = 3;
#ifdef WTPC_TUNE_PARAMS
        else if (strcmp(argv[i], "-T") == 0) mode = 4;
        else if (strcmp(argv[i], "-S") == 0 && i+1 < argc) tune_start = atoi(argv[++i]);
        else if (strcmp(argv[i], "-v") == 0) g_tune_verbose = 1;
        else if (strcmp(argv[i], "-420") == 0) tune_420 = 1;
#endif
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) output = argv[++i];
        else if (argv[i][0] != '-') input = argv[i];
    }

    if (mode == -1 || !input) {
        printf("Usage:\n");
        printf("  wtpc -e input.png -o output.wtp [-q QUALITY] [-c] [-b TARGET_BYTES]\n");
        printf("  wtpc -d input.wtp -o output.png\n");
        printf("  wtpc -t input.png [-q quality]   (self-test)\n");
        printf("  -e  encode mode\n");
        printf("  -d  decode mode\n");
        printf("  -q  quality 1 - %d (default 20)\n", MAX_QUALITY);
        printf("  -b  target size in bytes (auto-finds q, overrides -q)\n");
        printf("  -c  4:2:0 chroma subsampling\n");
        printf("  -m  best|ebcot|huffman  encoding mode (default: ebcot)\n");
        printf("  -h  context-aware Huffman tables (default: 0 = single table - faster, 1 - slower and slightly better compression ~35kb->36kb)\n");
        printf("  -G  generate huffman tables from images in directory\n");
#ifdef WTPC_TUNE_PARAMS
        printf("  -T  grid-search multipliers for tuning\n");
        printf("  -420  tune chroma 4:2:0 params (default: main luma+chroma444+DZ)\n");
        printf("  -S  start grid from param index (0..%d, default 0)\n", (tune_420 ? NPARAMS_420 : NPARAMS_MAIN)-1);
        printf("  -v  verbose: per-encode progress logging\n");
#endif
        printf("  -o  output file\n");
        return 1;
    }

    if (mode == 1) {
        /* Encode: auto-output = replace extension with .wtpc */
        char auto_out[1024];
        wtpc_enc_info info;
        if (!output) {
            const char *dot = strrchr(input, '.');
            if (dot) { int len = (int)(dot - input); snprintf(auto_out, sizeof(auto_out), "%.*s.wtpc", len, input); }
            else snprintf(auto_out, sizeof(auto_out), "%s.wtpc", input);
            output = auto_out;
        }
        int w, h, comp, has_alpha;
        uint8_t *img = NULL;
        {
            const char *ext = strrchr(input, '.');
            if (ext && strcmp(ext, ".png") == 0) {
                img = read_png(input, &w, &h, &has_alpha);
                if (!img) { printf("Cannot load: %s\n", input); return 1; }
            } else {
                img = stbi_load(input, &w, &h, &comp, 0);
                if (!img) { printf("Cannot load: %s\n", input); return 1; }
                has_alpha = (comp == 4);
            }
        }
        double t0 = now_ms();
        int ret = wtpc_encode_file(output, img, &info, w, h, target_bytes, quality, chroma_420, huffman_mode, huf_extra_ctx, has_alpha);
        double dt = now_ms() - t0;
        stbi_image_free(img);
        if (ret != 0) {
            printf("Encoding failed!\n"); return 1;
        }
        printf("Encoded %s -> %s (q=%d mode=%s%s%s result_mode=%s steps=%d) in %.3f ms\n", input, output, info.result_q, huffman_mode == 1 ? "huffman" : huffman_mode == 2 ? "ebcot" : "best", chroma_420 ? " 420":"", has_alpha ? " alpha":"", info.ebcot ? "ebcot" : "huffman", info.search_steps, dt);
        if (!info.ebcot)
            printf("  huffman  Y:tbl=%d(%s) U:tbl=%d(%s) V:tbl=%d(%s)  custom_bits=Y:%d U:%d V:%d\n",
                info.huffman_y_table, info.huffman_y_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_u_table, info.huffman_u_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_v_table, info.huffman_v_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_y_size, info.huffman_u_size, info.huffman_v_size);
    } else if (mode == 0) {
        /* Decode: auto-output = append .png */
        char auto_out[1024];
        if (!output) { snprintf(auto_out, sizeof(auto_out), "%s.png", input); output = auto_out; }
        double t0 = now_ms();
        int w, h, quality, comp;
        unsigned char *rgb = wtpc_decode_file(input, &w, &h, &quality, &comp);
        double dt = now_ms() - t0;
        if (!rgb) { printf("Decoding failed!\n"); return 1; }
        /* Detect format from extension: .png .bmp .tga .jpg */
        const char *ext = strrchr(output, '.');
        int ret = -1;
        if (ext && strcmp(ext, ".bmp") == 0) ret = stbi_write_bmp(output, w, h, comp, rgb) ? 0 : -1;
        else if (ext && strcmp(ext, ".tga") == 0) ret = stbi_write_tga(output, w, h, comp, rgb) ? 0 : -1;
        else if (ext && strcmp(ext, ".jpg") == 0) ret = stbi_write_jpg(output, w, h, comp, rgb, 90) ? 0 : -1;
        else ret = stbi_write_png(output, w, h, comp, rgb, w*comp) ? 0 : -1;
        free(rgb);
        if (ret != 0) { printf("Cannot write output: %s\n", output); return 1; }
        printf("Decoded %s -> %s (%d comp) in %.3f ms\n", input, output, comp, dt);
    } else if (mode == 3) {
        if (!input) { printf("Usage: wtpc -g <directory>\n"); return 1; }
        generate_tables(input);
#ifdef WTPC_TUNE_PARAMS
    } else if (mode == 4) {
        int nparams = tune_420 ? NPARAMS_420 : NPARAMS_MAIN;
        if (!input) { printf("Usage: wtpc -T <directory> [-420] [-S <start>] [-v]\n"); return 1; }
        if (tune_start < 0 || tune_start >= nparams) { printf("Invalid -S: must be 0..%d\n", nparams-1); return 1; }
        if (tune_420)
            tune_grid(input, param_cfg_420, NPARAMS_420, P420_BAND0, 1, tune_start);
        else
            tune_grid(input, param_cfg, NPARAMS_MAIN, 0, 0, tune_start);
#endif
    } else {
        /* Self-test: Huffman encode/decode roundtrip */
        int all_ok = 1;
        int16_t test_coeffs[1000];
        for(int i = 0; i < 1000; i++) test_coeffs[i] = 0;
        test_coeffs[0] = 5; test_coeffs[10] = -3; test_coeffs[50] = 120; test_coeffs[999] = 7;
        int freq[NUM_HUFF_SYMBOLS];
        count_frequencies(test_coeffs, 1000, freq);
        int cl[NUM_HUFF_SYMBOLS];
        uint32_t hc[NUM_HUFF_SYMBOLS];
        build_huffman_codes(freq, NUM_HUFF_SYMBOLS, cl);
        generate_canonical_codes(cl, NUM_HUFF_SYMBOLS, hc);
        uint8_t *enc_buf=(uint8_t*)malloc(10000);
        Bitstream bs; bitstream_init(&bs, enc_buf, 10000);
        huffman_encode_runval(&bs, test_coeffs, 1000, hc, cl);
        bitstream_flush(&bs);
        int enc_len = bitstream_bytes(&bs);
        int16_t *dec_coeffs = (int16_t*)calloc(1000, sizeof(int16_t));
        Bitstream rbs;
        bitstream_init(&rbs, enc_buf, enc_len);
        huffman_decode_channel(&rbs, dec_coeffs, 1000, cl, hc);
        int errors = 0;
        for(int i = 0; i < 1000; i++)
            if(test_coeffs[i] != dec_coeffs[i]) { if(errors<10) printf("[%d] %d!=%d\n", i, test_coeffs[i], dec_coeffs[i]); errors++; }
        if(errors == 0)
            printf("[SELF-TEST HUFFMAN] OK: 1000 coeffs, %d bytes\n", enc_len);
        else
            { printf("[SELF-TEST HUFFMAN] FAIL: %d mismatches\n", errors); all_ok = 0; }
        free(enc_buf); free(dec_coeffs);
        /* Self-test: EBCOT roundtrip (small block) */
        int w = 4, h = 4, total = 16;
        int16_t c[16] = {0,5,-3,0, 0,0,12,0, 0,-7,0,0, 1,0,0,0};
        int bp = 0, mv = 0;
        for(int i = 0; i < total; i++) { int av = abs(c[i]); if(av > mv) mv = av; }
        while(mv > 0) { bp++; mv >>= 1; }
        if(bp == 0) bp = 1;
        uint8_t *buf = (uint8_t*)malloc(4096);
        BacEnc e;
        bac_init_enc(&e, buf, 4096);
        ebcot_encode_channel(&e, c, w, h);
        int sz = bac_flush_enc(&e);
        int16_t *d=(int16_t*)calloc(total, sizeof(int16_t));
        BacDec dec;
        bac_init_dec(&dec, buf, sz);
        ebcot_decode_channel(&dec, d, w, h, bp);
        errors = 0;
        for(int i = 0; i < total;i++)
            if(c[i] != d[i]) { if(errors < 10) printf("[%d] %d!=%d\n", i, c[i], d[i]); errors++; }
        if(errors == 0)
            printf("[SELF-TEST EBCOT]  OK: 4x4 roundtrip, %d bytes\n", sz);
        else
            { printf("[SELF-TEST EBCOT]  FAIL: %d mismatches\n", errors); all_ok = 0; }
        free(buf); free(d);
        /* Stress-test: EBCOT roundtrip on pseudo-random coeffs w/ large range */
        {
            int sw = 48, sh = 48, st = sw*sh;
            int16_t *sc = (int16_t*)malloc(st*sizeof(int16_t));
            int16_t *sd = (int16_t*)calloc(st, sizeof(int16_t));
            unsigned rng = 12345, fails = 0;
            for(int trial = 0; trial < 200 && fails == 0; trial++) {
                int maxbits = 1 + (trial % 15);  /* 1..15-bit magnitudes; include tiny */
                for(int i = 0; i < st; i++){
                    rng = rng*1664525u + 1013904223u;
                    int mag = (rng >> 9) & ((1 << maxbits) - 1);
                    /* make it sparse like real wavelet coeffs */
                    if(((rng >> 3) & 7) < 5) mag = 0;
                    sc[i] = (rng & 0x100) ? -mag : mag;
                }
                int sbp=0, smv=0;
                for(int i = 0; i < st;i++) { int av = abs(sc[i]); if(av>smv) smv=av; }
                while(smv > 0) { sbp++; smv >>= 1; } if(sbp == 0) sbp=1;
                uint8_t *sbuf = (uint8_t*)malloc(st*6 + 4096);
                BacEnc se; bac_init_enc(&se, sbuf, st*6 + 4096);
                ebcot_encode_channel(&se, sc, sw, sh);
                int ssz=bac_flush_enc(&se);
                memset(sd, 0, st*sizeof(int16_t));
                BacDec sdec;
                bac_init_dec(&sdec, sbuf, ssz);
                ebcot_decode_channel(&sdec, sd, sw, sh, sbp);
                for(int i = 0; i < st; i++)
                    if(sc[i] != sd[i]) { fails++; if(fails <= 3) printf("[STRESS trial %d bits %d] idx %d: %d!=%d\n", trial, maxbits, i, sc[i], sd[i]); }
                free(sbuf);
            }
            if(fails == 0)
                printf("[SELF-TEST EBCOT-STRESS] OK: 200 trials 48x48 (BAC_CODE_BITS=%d)\n", BAC_CODE_BITS);
            else
                { printf("[SELF-TEST EBCOT-STRESS] FAIL: %u mismatches (BAC_CODE_BITS=%d)\n", fails, BAC_CODE_BITS); all_ok = 0; }
            free(sc); free(sd);
            }
        /* Self-test: wavelet roundtrip on non-square, non-power-of-2 (256x375) */
        {
            int tw = 256, th = 375, tt = tw*th;
            float *tf = (float*)malloc(tt*sizeof(float));
            float *ts = (float*)malloc(tt*sizeof(float));  /* save copy */
            unsigned rng = 42;
            for(int i = 0; i < tt; i++) {
                rng = rng*1664525u + 1013904223u;
                tf[i] = (float)((int)(rng & 0xFF) - 128);
                ts[i] = tf[i];
            }
            cdf97_forward_2d(tf, tw, th);
            cdf97_inverse_2d(tf, tw, th);
            float maxerr = 0;
            for(int i = 0; i < tt; i++){ float e = fabsf(tf[i] - ts[i]); if(e > maxerr) maxerr = e; }
            if(maxerr < 1.0f)
                printf("[SELF-TEST WAVELET]  OK: 256x375 roundtrip, maxerr=%.4f\n", maxerr);
            else
                { printf("[SELF-TEST WAVELET]  FAIL: 256x375 maxerr=%.4f\n", maxerr); all_ok = 0; }
            free(tf); free(ts);
        }
        /* Self-test: quantize roundtrip on non-square 256x375 */
        {
            int tw = 256, th = 375, tt = tw*th;
            float *ty = (float*)malloc(tt*sizeof(float));
            float *tu = (float*)malloc(tt*sizeof(float));
            float *tv = (float*)malloc(tt*sizeof(float));
            uint8_t *orig = (uint8_t*)malloc(tt*3);
            uint8_t *dec = (uint8_t*)malloc(tt*3);
            unsigned rng = 123;
            for(int i = 0; i < tt; i++) {
                rng = rng*1664525u + 1013904223u;
                orig[i*3] = rng & 0xFF; orig[i*3 + 1] = (rng >> 8) & 0xFF; orig[i*3 + 2] = (rng >> 16) & 0xFF;
                rgb_to_yuv(orig[i*3],orig[i*3 + 1],orig[i*3 + 2], &ty[i], &tu[i], &tv[i]);
            }
            cdf97_forward_2d(ty, tw, th);
            cdf97_forward_2d(tu, tw, th);
            cdf97_forward_2d(tv, tw, th);
            int16_t *qy = (int16_t*)malloc(tt*2), *qu = (int16_t*)malloc(tt*2), *qv = (int16_t*)malloc(tt*2);
            quantize_coeffs(ty, qy, tw, th, compute_base(1), g_quant_y); quantize_coeffs(tu, qu, tw, th, compute_base(1), g_quant_c); quantize_coeffs(tv, qv, tw, th, compute_base(1), g_quant_c);
            dequantize_channel(qy, ty, tw, th, compute_base(1), g_quant_y); dequantize_channel(qu, tu, tw, th, compute_base(1), g_quant_c); dequantize_channel(qv, tv, tw, th, compute_base(1), g_quant_c);
            cdf97_inverse_2d(ty, tw, th);
            cdf97_inverse_2d(tu, tw, th);
            cdf97_inverse_2d(tv, tw, th);
            for(int i = 0; i < tt; i++)
                yuv_to_rgb(ty[i], tu[i], tv[i], &dec[i*3], &dec[i*3+1], &dec[i*3+2]);
            double mse=0;
            for(int i = 0; i < tt*3; i++){ double d = orig[i] - dec[i]; mse += d*d; }
            mse /= (tt*3);
            double psnr = 10.0*log10(255.0*255.0/mse);
            if(psnr > 38.0)
                printf("[SELF-TEST Q-ROUNDTRIP] OK: 256x375 psnr=%.2f dB\n", psnr);
            else
                { printf("[SELF-TEST Q-ROUNDTRIP] FAIL: 256x375 psnr=%.2f dB (mse=%.1f)\n", psnr, mse); all_ok = 0; }
            free(ty); free(tu); free(tv); free(orig); free(dec); free(qy); free(qu); free(qv);
            /* Same test on 256x256 (known-good square) */
            tw = 256; th = 256; tt = tw*th;
            ty = (float*)malloc(tt*sizeof(float));
            tu = (float*)malloc(tt*sizeof(float));
            tv = (float*)malloc(tt*sizeof(float));
            orig = (uint8_t*)malloc(tt*3);
            dec = (uint8_t*)malloc(tt*3);
            rng = 123;
            for(int i = 0; i < tt; i++){
                rng = rng*1664525u + 1013904223u;
                orig[i*3] = rng&0xFF; orig[i*3 + 1] = (rng >> 8) & 0xFF; orig[i*3 + 2] = (rng >> 16) & 0xFF;
                rgb_to_yuv(orig[i*3], orig[i*3 + 1], orig[i*3 + 2], &ty[i], &tu[i], &tv[i]);
            }
            cdf97_forward_2d(ty, tw, th);
            cdf97_forward_2d(tu, tw, th);
            cdf97_forward_2d(tv, tw, th);
            qy = (int16_t*)malloc(tt*2); qu=(int16_t*)malloc(tt*2); qv=(int16_t*)malloc(tt*2);
            quantize_coeffs(ty, qy, tw, th, compute_base(1), g_quant_y);
            quantize_coeffs(tu, qu, tw, th, compute_base(1), g_quant_c);
            quantize_coeffs(tv, qv, tw, th, compute_base(1), g_quant_c);
            dequantize_channel(qy, ty, tw, th, compute_base(1), g_quant_y);
            dequantize_channel(qu, tu, tw, th, compute_base(1), g_quant_c);
            dequantize_channel(qv, tv, tw, th, compute_base(1), g_quant_c);
            cdf97_inverse_2d(ty, tw, th);
            cdf97_inverse_2d(tu, tw, th);
            cdf97_inverse_2d(tv, tw, th);
            for(int i = 0; i < tt; i++)
                yuv_to_rgb(ty[i], tu[i], tv[i], &dec[i*3], &dec[i*3 + 1], &dec[i*3 + 2]);
            mse = 0;
            for(int i = 0; i < tt*3; i++) { double d = orig[i] - dec[i]; mse += d*d; }
            mse /= (tt*3);
            psnr = 10.0*log10(255.0*255.0/mse);
            if(psnr > 38.0)
                printf("[SELF-TEST Q-ROUNDTRIP] OK: 256x256 psnr=%.2f dB\n", psnr);
            else
                { printf("[SELF-TEST Q-ROUNDTRIP] FAIL: 256x256 psnr=%.2f dB (mse=%.1f)\n", psnr, mse); all_ok = 0; }
            free(ty); free(tu); free(tv); free(orig); free(dec); free(qy); free(qu); free(qv);
        }
        if(!all_ok) return 1;
    }
    return 0;
}
