#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <png.h>
#else
#include <windows.h>
#include <mmsystem.h>
#define snprintf _snprintf
#endif

#define WTPC_IMAGE_IMPLEMENTATION
#include "wtpc_image.h"
#ifdef _WIN32
#ifdef WTPC_TUNE_PARAMS
#undef WTPC_TUNE_PARAMS
#endif
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef WTPC_TUNE_PARAMS
#define TUNE_THREADS 7
#define MAX_WORSE_STEPS 3
#define MAX_WORSE_SSIM  0.5f
static int g_tune_verbose = 0;
#endif

static double now_ms(void) {
#ifdef _WIN32
    return timeGetTime();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

#ifndef _WIN32    
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
static void put(int64_t f[NUM_HUFF_SYMBOLS]) {
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

static void put_set(int64_t freq[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS]) {
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

static void generate_tables(const char *dir_path) {
    int q_levels[NUM_DEF_TABLES] = {664, 566, 463, 351, 222, 119, 56}; /* from mean_q= for each target after quantization tune */

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
        int w, h, has_alpha = 0;
        uint8_t *rgb;
        if (ext && strcmp(ext, ".png") == 0) {
            rgb = read_png(path, &w, &h, &has_alpha);
        } else {
            int comp;
            rgb = stbi_load(path, &w, &h, &comp, 0);
            has_alpha = (comp == 4);
        }
        if (!rgb) continue;

        size_t total = (size_t)w * h; int stride = has_alpha ? 4 : 3;
        float *y_w = malloc(total * sizeof(float));
        float *u_w = malloc(total * sizeof(float));
        float *v_w = malloc(total * sizeof(float));
        int16_t *q_y = (int16_t*)malloc((size_t)(w+2)*(h+2) * sizeof(int16_t));
        int16_t *q_u = (int16_t*)malloc((size_t)(w+2)*(h+2) * sizeof(int16_t));
        int16_t *q_v = (int16_t*)malloc((size_t)(w+2)*(h+2) * sizeof(int16_t));
        float *a_w = NULL; int16_t *q_a = NULL;
        if (has_alpha) {
            a_w = malloc(total * sizeof(float));
            q_a = (int16_t*)malloc((size_t)(w+2)*(h+2) * sizeof(int16_t));
        }
        if (!y_w || !u_w || !v_w || !q_y || !q_u || !q_v
            || (has_alpha && (!a_w || !q_a))) {
            free(y_w); free(u_w); free(v_w); free(q_y); free(q_u); free(q_v);
            free(a_w); free(q_a);
            free(rgb); continue;
        }
        for (size_t i = 0; i < total; ++i)
            rgb_to_yuv(rgb[i*stride], rgb[i*stride+1], rgb[i*stride+2], &y_w[i], &u_w[i], &v_w[i]);
        if (has_alpha) {
            for (size_t i = 0; i < total; i++) a_w[i] = (float)rgb[i*4 + 3] - 128.0f;
        }
        free(rgb);

        /* 4:2:0 path: downsample chroma BEFORE wavelet (u_w/v_w still in pixel domain) */
        int cw420 = (w+1)/2, ch420 = (h+1)/2, ctotal420 = cw420 * ch420;
        float *u_ds = malloc(ctotal420 * sizeof(float));
        float *v_ds = malloc(ctotal420 * sizeof(float));
        int16_t *q_u420 = (int16_t*)malloc((size_t)(cw420+2)*(ch420+2) * sizeof(int16_t));
        int16_t *q_v420 = (int16_t*)malloc((size_t)(cw420+2)*(ch420+2) * sizeof(int16_t));
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
        if (has_alpha) cdf97_forward_2d(a_w, w, h);

        for (int lev = 0; lev < NUM_DEF_TABLES; lev++) {
            int q = q_levels[lev];
            float base = compute_base(q);

            /* --- 4:4:4: quantize (padded) + count --- */
            quantize_coeffs(y_w, q_y, w, h, base, g_qt_y);
            quantize_coeffs(u_w, q_u, w, h, base, g_qt_c);
            quantize_coeffs(v_w, q_v, w, h, base, g_qt_c);

            int fs[NUM_HUFF_SYMBOLS];
            count_frequencies(q_y, w, h, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][0][s] += fs[s];
            count_frequencies(q_u, w, h, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][1][s] += fs[s];
            count_frequencies(q_v, w, h, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][2][s] += fs[s];

            int f0[NUM_HUFF_SYMBOLS], f1[NUM_HUFF_SYMBOLS];
            count_frequencies_ctx(q_y, w, h, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][0][s] += f0[s]; freq_t1[lev][0][s] += f1[s]; }
            count_frequencies_ctx(q_u, w, h, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][1][s] += f0[s]; freq_t1[lev][1][s] += f1[s]; }
            count_frequencies_ctx(q_v, w, h, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][2][s] += f0[s]; freq_t1[lev][2][s] += f1[s]; }
            if (has_alpha) {
                quantize_coeffs(a_w, q_a, w, h, base, g_qt_y);
                count_frequencies(q_a, w, h, fs);
                for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single[lev][0][s] += fs[s];
                count_frequencies_ctx(q_a, w, h, f0, f1);
                for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0[lev][0][s] += f0[s]; freq_t1[lev][0][s] += f1[s]; }
            }

            /* --- 4:2:0: quantize half-res U/V, Y already quantized from 4:4:4 --- */
            quantize_coeffs(u_ds, q_u420, cw420, ch420, base, g_qt_c420);
            quantize_coeffs(v_ds, q_v420, cw420, ch420, base, g_qt_c420);

            /* Y: recount from q_y (fs/f0/f1 were overwritten by U/V above), then accumulate */
            count_frequencies(q_y, w, h, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][0][s] += fs[s];
            count_frequencies(q_u420, cw420, ch420, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][1][s] += fs[s];
            count_frequencies(q_v420, cw420, ch420, fs);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][2][s] += fs[s];

            count_frequencies_ctx(q_y, w, h, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][0][s] += f0[s]; freq_t1_420[lev][0][s] += f1[s]; }
            count_frequencies_ctx(q_u420, cw420, ch420, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][1][s] += f0[s]; freq_t1_420[lev][1][s] += f1[s]; }
            count_frequencies_ctx(q_v420, cw420, ch420, f0, f1);
            for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][2][s] += f0[s]; freq_t1_420[lev][2][s] += f1[s]; }
            if (has_alpha) {
                count_frequencies(q_a, w, h, fs);
                for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) freq_single_420[lev][0][s] += fs[s];
                count_frequencies_ctx(q_a, w, h, f0, f1);
                for (int s = 0; s < NUM_HUFF_SYMBOLS; s++) { freq_t0_420[lev][0][s] += f0[s]; freq_t1_420[lev][0][s] += f1[s]; }
            }
        }
        free(y_w); free(u_w); free(v_w); free(q_y); free(q_u); free(q_v);
        free(a_w); free(q_a);
        free(u_ds); free(v_ds); free(q_u420); free(q_v420);
        img_count++;
        fprintf(stderr, "\r%d images", img_count); fflush(stderr);
    }
    closedir(d);
    fprintf(stderr, "\nTotal: %d images\n", img_count);
    if (img_count == 0) return;

    printf("// Auto-generated Huffman tables from %d images\n", img_count);
    printf("// Quality levels: {%d,%d,%d,%d,%d,%d,%d}\n",
        q_levels[0], q_levels[1], q_levels[2], q_levels[3], q_levels[4], q_levels[5], q_levels[6]);
    printf("\n");
    printf("/* Single-table (huf_extra_ctx=0): all coefficients */\n");
    printf("static const uint8_t def_tables_single[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_single);
    printf("};\n\n");
    printf("/* t0 tables (prev-cat <= 2) */\n");
    printf("static const uint8_t def_tables_t0[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t0);
    printf("};\n\n");
    printf("/* t1 tables (prev-cat > 2) */\n");
    printf("static const uint8_t def_tables_t1[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t1);
    printf("};\n\n");
    printf("/* 4:2:0 single-table (huf_extra_ctx=0): all coefficients */\n");
    printf("static const uint8_t def_tables_single_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_single_420);
    printf("};\n\n");
    printf("/* 4:2:0 t0 tables (prev-cat <= 2) */\n");
    printf("static const uint8_t def_tables_t0_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t0_420);
    printf("};\n\n");
    printf("/* 4:2:0 t1 tables (prev-cat > 2) */\n");
    printf("static const uint8_t def_tables_t1_420[NUM_DEF_TABLES][3][NUM_HUFF_SYMBOLS] = {\n");
    put_set(freq_t1_420);
    printf("};\n");
}
#endif /* !_WIN32 */

#ifdef WTPC_TUNE_PARAMS

enum {
    /* --- Y: LH/HL/HH multipliers + per-subband DZ (48 params) --- */
    P_Y_LH0,P_Y_LH1,P_Y_LH2,P_Y_LH3,P_Y_LH4,P_Y_LH5,P_Y_LH6,P_Y_LH7,
    P_Y_HL0,P_Y_HL1,P_Y_HL2,P_Y_HL3,P_Y_HL4,P_Y_HL5,P_Y_HL6,P_Y_HL7,
    P_Y_HH0,P_Y_HH1,P_Y_HH2,P_Y_HH3,P_Y_HH4,P_Y_HH5,P_Y_HH6,P_Y_HH7,
    P_Y_DZ_LH0,P_Y_DZ_LH1,P_Y_DZ_LH2,P_Y_DZ_LH3,P_Y_DZ_LH4,P_Y_DZ_LH5,P_Y_DZ_LH6,P_Y_DZ_LH7,
    P_Y_DZ_HL0,P_Y_DZ_HL1,P_Y_DZ_HL2,P_Y_DZ_HL3,P_Y_DZ_HL4,P_Y_DZ_HL5,P_Y_DZ_HL6,P_Y_DZ_HL7,
    P_Y_DZ_HH0,P_Y_DZ_HH1,P_Y_DZ_HH2,P_Y_DZ_HH3,P_Y_DZ_HH4,P_Y_DZ_HH5,P_Y_DZ_HH6,P_Y_DZ_HH7,
    /* --- C 4:4:4: LH/HL/HH multipliers + per-subband DZ (48 params) --- */
    P_C_LH0,P_C_LH1,P_C_LH2,P_C_LH3,P_C_LH4,P_C_LH5,P_C_LH6,P_C_LH7,
    P_C_HL0,P_C_HL1,P_C_HL2,P_C_HL3,P_C_HL4,P_C_HL5,P_C_HL6,P_C_HL7,
    P_C_HH0,P_C_HH1,P_C_HH2,P_C_HH3,P_C_HH4,P_C_HH5,P_C_HH6,P_C_HH7,
    P_C_DZ_LH0,P_C_DZ_LH1,P_C_DZ_LH2,P_C_DZ_LH3,P_C_DZ_LH4,P_C_DZ_LH5,P_C_DZ_LH6,P_C_DZ_LH7,
    P_C_DZ_HL0,P_C_DZ_HL1,P_C_DZ_HL2,P_C_DZ_HL3,P_C_DZ_HL4,P_C_DZ_HL5,P_C_DZ_HL6,P_C_DZ_HL7,
    P_C_DZ_HH0,P_C_DZ_HH1,P_C_DZ_HH2,P_C_DZ_HH3,P_C_DZ_HH4,P_C_DZ_HH5,P_C_DZ_HH6,P_C_DZ_HH7,
    /* --- DC (Y + C, stored in LH[16]) --- */
    P_DC_Y, P_DC_C,
    /* --- EXT multipliers for bands 8+ (Y + C) --- */
    P_EXT_MULT_Y, P_EXT_MULT_HH_Y, P_EXT_MULT_C, P_EXT_MULT_HH_C,
    /* --- C420: LH/HL/HH multipliers + per-subband DZ + DC (49 params) --- */
    P_C420_LH0,P_C420_LH1,P_C420_LH2,P_C420_LH3,P_C420_LH4,P_C420_LH5,P_C420_LH6,P_C420_LH7,
    P_C420_HL0,P_C420_HL1,P_C420_HL2,P_C420_HL3,P_C420_HL4,P_C420_HL5,P_C420_HL6,P_C420_HL7,
    P_C420_HH0,P_C420_HH1,P_C420_HH2,P_C420_HH3,P_C420_HH4,P_C420_HH5,P_C420_HH6,P_C420_HH7,
    P_C420_DZ_LH0,P_C420_DZ_LH1,P_C420_DZ_LH2,P_C420_DZ_LH3,P_C420_DZ_LH4,P_C420_DZ_LH5,P_C420_DZ_LH6,P_C420_DZ_LH7,
    P_C420_DZ_HL0,P_C420_DZ_HL1,P_C420_DZ_HL2,P_C420_DZ_HL3,P_C420_DZ_HL4,P_C420_DZ_HL5,P_C420_DZ_HL6,P_C420_DZ_HL7,
    P_C420_DZ_HH0,P_C420_DZ_HH1,P_C420_DZ_HH2,P_C420_DZ_HH3,P_C420_DZ_HH4,P_C420_DZ_HH5,P_C420_DZ_HH6,P_C420_DZ_HH7,
    P_DC_C420,
    P_EXT_MULT_C420, P_EXT_MULT_HH_C420,
    NPARAMS
};
#define NPARAMS_MAIN (P_DC_C + 1)
#define NPARAMS_420  (P_DC_C420 + 1 - P_C420_LH0)
#define NPARAMS_EXT  6

typedef struct {
    const char *name;
    float delta;
    float win_size;
} ParamCfg;

#define BAND_NAME(sb, b) ((b)==0 ? sb"0(c)" : (b)==7 ? sb"7(f)" : sb #b)

static const ParamCfg param_cfg[NPARAMS_MAIN] = {
    /* Y LH */
    {BAND_NAME("y_lh",0),0.01f,0.05f},{BAND_NAME("y_lh",1),0.01f,0.05f},{BAND_NAME("y_lh",2),0.01f,0.05f},{BAND_NAME("y_lh",3),0.01f,0.05f},
    {BAND_NAME("y_lh",4),0.01f,0.05f},{BAND_NAME("y_lh",5),0.01f,0.05f},{BAND_NAME("y_lh",6),0.01f,0.10f},{BAND_NAME("y_lh",7),0.01f,0.20f},
    /* Y HL */
    {BAND_NAME("y_hl",0),0.01f,0.05f},{BAND_NAME("y_hl",1),0.01f,0.05f},{BAND_NAME("y_hl",2),0.01f,0.05f},{BAND_NAME("y_hl",3),0.01f,0.05f},
    {BAND_NAME("y_hl",4),0.01f,0.05f},{BAND_NAME("y_hl",5),0.01f,0.05f},{BAND_NAME("y_hl",6),0.01f,0.10f},{BAND_NAME("y_hl",7),0.01f,0.20f},
    /* Y HH */
    {BAND_NAME("y_hh",0),0.01f,0.05f},{BAND_NAME("y_hh",1),0.01f,0.05f},{BAND_NAME("y_hh",2),0.01f,0.05f},{BAND_NAME("y_hh",3),0.01f,0.05f},
    {BAND_NAME("y_hh",4),0.01f,0.05f},{BAND_NAME("y_hh",5),0.01f,0.05f},{BAND_NAME("y_hh",6),0.01f,0.10f},{BAND_NAME("y_hh",7),0.01f,0.20f},
    /* Y DZ LH */
    {BAND_NAME("y_dzl",0),0.01f,0.05f},{BAND_NAME("y_dzl",1),0.01f,0.05f},{BAND_NAME("y_dzl",2),0.01f,0.05f},{BAND_NAME("y_dzl",3),0.01f,0.05f},
    {BAND_NAME("y_dzl",4),0.01f,0.05f},{BAND_NAME("y_dzl",5),0.01f,0.05f},{BAND_NAME("y_dzl",6),0.01f,0.10f},{BAND_NAME("y_dzl",7),0.01f,0.20f},
    /* Y DZ HL */
    {BAND_NAME("y_dzh",0),0.01f,0.05f},{BAND_NAME("y_dzh",1),0.01f,0.05f},{BAND_NAME("y_dzh",2),0.01f,0.05f},{BAND_NAME("y_dzh",3),0.01f,0.05f},
    {BAND_NAME("y_dzh",4),0.01f,0.05f},{BAND_NAME("y_dzh",5),0.01f,0.05f},{BAND_NAME("y_dzh",6),0.01f,0.10f},{BAND_NAME("y_dzh",7),0.01f,0.20f},
    /* Y DZ HH */
    {BAND_NAME("y_dzH",0),0.01f,0.05f},{BAND_NAME("y_dzH",1),0.01f,0.05f},{BAND_NAME("y_dzH",2),0.01f,0.05f},{BAND_NAME("y_dzH",3),0.01f,0.05f},
    {BAND_NAME("y_dzH",4),0.01f,0.05f},{BAND_NAME("y_dzH",5),0.01f,0.05f},{BAND_NAME("y_dzH",6),0.01f,0.10f},{BAND_NAME("y_dzH",7),0.01f,0.20f},
    /* C 4:4:4 LH */
    {BAND_NAME("c_lh",0),0.01f,0.10f},{BAND_NAME("c_lh",1),0.01f,0.10f},{BAND_NAME("c_lh",2),0.01f,0.10f},{BAND_NAME("c_lh",3),0.01f,0.10f},
    {BAND_NAME("c_lh",4),0.01f,0.10f},{BAND_NAME("c_lh",5),0.01f,0.10f},{BAND_NAME("c_lh",6),0.01f,0.20f},{BAND_NAME("c_lh",7),0.01f,0.40f},
    /* C 4:4:4 HL */
    {BAND_NAME("c_hl",0),0.01f,0.10f},{BAND_NAME("c_hl",1),0.01f,0.10f},{BAND_NAME("c_hl",2),0.01f,0.10f},{BAND_NAME("c_hl",3),0.01f,0.10f},
    {BAND_NAME("c_hl",4),0.01f,0.10f},{BAND_NAME("c_hl",5),0.01f,0.10f},{BAND_NAME("c_hl",6),0.01f,0.20f},{BAND_NAME("c_hl",7),0.01f,0.40f},
    /* C 4:4:4 HH */
    {BAND_NAME("c_hh",0),0.01f,0.10f},{BAND_NAME("c_hh",1),0.01f,0.10f},{BAND_NAME("c_hh",2),0.01f,0.10f},{BAND_NAME("c_hh",3),0.01f,0.10f},
    {BAND_NAME("c_hh",4),0.01f,0.10f},{BAND_NAME("c_hh",5),0.01f,0.10f},{BAND_NAME("c_hh",6),0.01f,0.20f},{BAND_NAME("c_hh",7),0.01f,0.40f},
    /* C 4:4:4 DZ LH */
    {BAND_NAME("c_dzl",0),0.01f,0.05f},{BAND_NAME("c_dzl",1),0.01f,0.05f},{BAND_NAME("c_dzl",2),0.01f,0.05f},{BAND_NAME("c_dzl",3),0.01f,0.05f},
    {BAND_NAME("c_dzl",4),0.01f,0.05f},{BAND_NAME("c_dzl",5),0.01f,0.05f},{BAND_NAME("c_dzl",6),0.01f,0.10f},{BAND_NAME("c_dzl",7),0.01f,0.20f},
    /* C 4:4:4 DZ HL */
    {BAND_NAME("c_dzh",0),0.01f,0.05f},{BAND_NAME("c_dzh",1),0.01f,0.05f},{BAND_NAME("c_dzh",2),0.01f,0.05f},{BAND_NAME("c_dzh",3),0.01f,0.05f},
    {BAND_NAME("c_dzh",4),0.01f,0.05f},{BAND_NAME("c_dzh",5),0.01f,0.05f},{BAND_NAME("c_dzh",6),0.01f,0.10f},{BAND_NAME("c_dzh",7),0.01f,0.20f},
    /* C 4:4:4 DZ HH */
    {BAND_NAME("c_dzH",0),0.01f,0.05f},{BAND_NAME("c_dzH",1),0.01f,0.05f},{BAND_NAME("c_dzH",2),0.01f,0.05f},{BAND_NAME("c_dzH",3),0.01f,0.05f},
    {BAND_NAME("c_dzH",4),0.01f,0.05f},{BAND_NAME("c_dzH",5),0.01f,0.05f},{BAND_NAME("c_dzH",6),0.01f,0.10f},{BAND_NAME("c_dzH",7),0.01f,0.20f},
    /* DC */
    {"dc_y",0.01f,0.05f},{"dc_c",0.01f,0.05f},
};

static const ParamCfg param_cfg_420[NPARAMS_420] = {
    /* C420 LH */
    {BAND_NAME("c4_lh",0),0.01f,0.8f},{BAND_NAME("c4_lh",1),0.01f,0.8f},{BAND_NAME("c4_lh",2),0.01f,0.8f},{BAND_NAME("c4_lh",3),0.01f,0.8f},
    {BAND_NAME("c4_lh",4),0.01f,0.8f},{BAND_NAME("c4_lh",5),0.01f,0.8f},{BAND_NAME("c4_lh",6),0.01f,1.0f},{BAND_NAME("c4_lh",7),0.01f,1.5f},
    /* C420 HL */
    {BAND_NAME("c4_hl",0),0.01f,0.8f},{BAND_NAME("c4_hl",1),0.01f,0.8f},{BAND_NAME("c4_hl",2),0.01f,0.8f},{BAND_NAME("c4_hl",3),0.01f,0.8f},
    {BAND_NAME("c4_hl",4),0.01f,0.8f},{BAND_NAME("c4_hl",5),0.01f,0.8f},{BAND_NAME("c4_hl",6),0.01f,1.0f},{BAND_NAME("c4_hl",7),0.01f,1.5f},
    /* C420 HH */
    {BAND_NAME("c4_hh",0),0.01f,0.8f},{BAND_NAME("c4_hh",1),0.01f,0.8f},{BAND_NAME("c4_hh",2),0.01f,0.8f},{BAND_NAME("c4_hh",3),0.01f,0.8f},
    {BAND_NAME("c4_hh",4),0.01f,0.8f},{BAND_NAME("c4_hh",5),0.01f,0.8f},{BAND_NAME("c4_hh",6),0.01f,1.0f},{BAND_NAME("c4_hh",7),0.01f,1.5f},
    /* C420 DZ LH */
    {BAND_NAME("c4dzl",0),0.01f,0.05f},{BAND_NAME("c4dzl",1),0.01f,0.05f},{BAND_NAME("c4dzl",2),0.01f,0.05f},{BAND_NAME("c4dzl",3),0.01f,0.05f},
    {BAND_NAME("c4dzl",4),0.01f,0.05f},{BAND_NAME("c4dzl",5),0.01f,0.05f},{BAND_NAME("c4dzl",6),0.01f,0.10f},{BAND_NAME("c4dzl",7),0.01f,0.20f},
    /* C420 DZ HL */
    {BAND_NAME("c4dzh",0),0.01f,0.05f},{BAND_NAME("c4dzh",1),0.01f,0.05f},{BAND_NAME("c4dzh",2),0.01f,0.05f},{BAND_NAME("c4dzh",3),0.01f,0.05f},
    {BAND_NAME("c4dzh",4),0.01f,0.05f},{BAND_NAME("c4dzh",5),0.01f,0.05f},{BAND_NAME("c4dzh",6),0.01f,0.10f},{BAND_NAME("c4dzh",7),0.01f,0.20f},
    /* C420 DZ HH */
    {BAND_NAME("c4dzH",0),0.01f,0.05f},{BAND_NAME("c4dzH",1),0.01f,0.05f},{BAND_NAME("c4dzH",2),0.01f,0.05f},{BAND_NAME("c4dzH",3),0.01f,0.05f},
    {BAND_NAME("c4dzH",4),0.01f,0.05f},{BAND_NAME("c4dzH",5),0.01f,0.05f},{BAND_NAME("c4dzH",6),0.01f,0.10f},{BAND_NAME("c4dzH",7),0.01f,0.20f},
    /* C420 DC */
    {"dc_c420",0.01f,0.05f},
};

static const ParamCfg param_cfg_ext[NPARAMS_EXT] = {
    {"ext_y",0.01f,0.20f},{"ext_hh_y",0.01f,0.20f},
    {"ext_c",0.01f,0.20f},{"ext_hh_c",0.01f,0.20f},
    {"ext_c420",0.01f,0.20f},{"ext_hh_c420",0.01f,0.20f},
};

static float g_params[NPARAMS];

static void apply_params(void) {
    /* Y multipliers */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_lh[b] = g_params[P_Y_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_hl[b] = g_params[P_Y_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_hh[b] = g_params[P_Y_HH0 + b];
    /* Y DZ */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_lh[MAX_BANDS+b] = g_params[P_Y_DZ_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_hl[MAX_BANDS+b] = g_params[P_Y_DZ_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_y_hh[MAX_BANDS+b] = g_params[P_Y_DZ_HH0 + b];
    /* C multipliers */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_lh[b] = g_params[P_C_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_hl[b] = g_params[P_C_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_hh[b] = g_params[P_C_HH0 + b];
    /* C DZ */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_lh[MAX_BANDS+b] = g_params[P_C_DZ_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_hl[MAX_BANDS+b] = g_params[P_C_DZ_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c_hh[MAX_BANDS+b] = g_params[P_C_DZ_HH0 + b];
    /* C420 multipliers */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_lh[b] = g_params[P_C420_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_hl[b] = g_params[P_C420_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_hh[b] = g_params[P_C420_HH0 + b];
    /* C420 DZ */
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_lh[MAX_BANDS+b] = g_params[P_C420_DZ_LH0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_hl[MAX_BANDS+b] = g_params[P_C420_DZ_HL0 + b];
    for (int b = 0; b < MAX_BANDS; b++) g_quant_c420_hh[MAX_BANDS+b] = g_params[P_C420_DZ_HH0 + b];
    /* DC */
    g_quant_y_lh[MAX_BANDS*2] = g_params[P_DC_Y];
    g_quant_c_lh[MAX_BANDS*2] = g_params[P_DC_C];
    g_quant_c420_lh[MAX_BANDS*2] = g_params[P_DC_C420];
    /* EXT multipliers for bands 8+ */
    g_quant_y_lh[MAX_BANDS*2+1] = g_params[P_EXT_MULT_Y];
    g_quant_y_lh[MAX_BANDS*2+2] = g_params[P_EXT_MULT_HH_Y];
    g_quant_c_lh[MAX_BANDS*2+1] = g_params[P_EXT_MULT_C];
    g_quant_c_lh[MAX_BANDS*2+2] = g_params[P_EXT_MULT_HH_C];
    g_quant_c420_lh[MAX_BANDS*2+1] = g_params[P_EXT_MULT_C420];
    g_quant_c420_lh[MAX_BANDS*2+2] = g_params[P_EXT_MULT_HH_C420];
}

static void print_all_params(int chroma) {
    if (!chroma) {
        printf("static WTPC_TABLES_CONST float g_quant_y_lh[MAX_BANDS*2+3] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_lh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_lh[MAX_BANDS+b]);
        printf(", /*DC*/ %.2ff, /*EXT*/ %.2ff, /*EXT_HH*/ %.2ff};\n", g_quant_y_lh[MAX_BANDS*2], g_quant_y_lh[MAX_BANDS*2+1], g_quant_y_lh[MAX_BANDS*2+2]);
        printf("static WTPC_TABLES_CONST float g_quant_y_hl[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_hl[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_hl[MAX_BANDS+b]);
        printf("};\n");
        printf("static WTPC_TABLES_CONST float g_quant_y_hh[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_hh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_y_hh[MAX_BANDS+b]);
        printf("};\n");
        printf("static WTPC_TABLES_CONST float g_quant_c_lh[MAX_BANDS*2+3] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_lh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_lh[MAX_BANDS+b]);
        printf(", /*DC*/ %.2ff, /*EXT*/ %.2ff, /*EXT_HH*/ %.2ff};\n", g_quant_c_lh[MAX_BANDS*2], g_quant_c_lh[MAX_BANDS*2+1], g_quant_c_lh[MAX_BANDS*2+2]);
        printf("static WTPC_TABLES_CONST float g_quant_c_hl[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_hl[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_hl[MAX_BANDS+b]);
        printf("};\n");
        printf("static WTPC_TABLES_CONST float g_quant_c_hh[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_hh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c_hh[MAX_BANDS+b]);
        printf("};\n");
    } else {
        printf("static WTPC_TABLES_CONST float g_quant_c420_lh[MAX_BANDS*2+3] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_lh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_lh[MAX_BANDS+b]);
        printf(", /*DC*/ %.2ff, /*EXT*/ %.2ff, /*EXT_HH*/ %.2ff};\n", g_quant_c420_lh[MAX_BANDS*2], g_quant_c420_lh[MAX_BANDS*2+1], g_quant_c420_lh[MAX_BANDS*2+2]);
        printf("static WTPC_TABLES_CONST float g_quant_c420_hl[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_hl[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_hl[MAX_BANDS+b]);
        printf("};\n");
        printf("static WTPC_TABLES_CONST float g_quant_c420_hh[MAX_BANDS*2] = {");
        for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_hh[b]);
        printf(", /*DZ*/ "); for (int b = 0; b < MAX_BANDS; b++) printf("%s%.2ff", b ? "," : "", g_quant_c420_hh[MAX_BANDS+b]);
        printf("};\n");
    }
    fflush(stdout);
}

typedef struct {
    uint8_t *rgb;
    int w, h;
    int has_alpha;
} PreloadedImg;

/* Per-target aggregate: one instance per (thread, target) pair, */
/* merged across threads after workers join.                     */
typedef struct {
    double ssim_sum;   /* sum of ssimulacra2 scores */
    int    count;      /* number of valid measurements */
    int    max_dev;    /* max absolute deviation (bytes) */
    double dev_sum;    /* sum of absolute deviations (bytes) */
    double q_sum;      /* sum of quality levels */
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
    const int *targets;
    int ntargets;
    int ntotal;            /* nimg * ntargets */
    volatile int *guard_overshoot;  /* shared flag set by any thread on guard fail */
    int skip_guard;        /* disable guard check (large-image EXT tuning) */
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
        if (idx >= ctx->ntotal || *ctx->guard_overshoot) break;

        /* Single chroma mode: idx -> (t, i) */
        int t    = idx / ctx->nimg;
        int i    = idx % ctx->nimg;
        int tidx = t;

        PreloadedImg *pimg = &ctx->images[i];
        int w = pimg->w, h = pimg->h;
        uint8_t *img = pimg->rgb;
#define GUARD_SIZE 150
        wtpc_enc_info info;
        int guard_size = 0, exclude_guard_test = !strcmp(ctx->names[i], "sample-alpha-checker-400x300.png") || !strcmp(ctx->names[i], "sample-alpha-circle-400x300.png");
        unsigned char *enc;
        if (!ctx->skip_guard && !exclude_guard_test) {
            enc = wtpc_encode_mem(img, &info, w, h, 0, MAX_QUALITY, ctx->chroma_mode, 2, 0, pimg->has_alpha, 0, 0);
            guard_size = info.encoded_bytes;
            free(enc);
        }
        enc = wtpc_encode_mem(img, &info, w, h, ctx->targets[t], 0, ctx->chroma_mode, 2, 0, pimg->has_alpha, 0, 0);
        if (!enc) {
            fprintf(stderr, "[th%d] BAD ENCODE %s: %dx%dx%d\n", ctx->tid, ctx->names[i], w, h, pimg->has_alpha ? 4 : 3); continue;
        }
        if (!ctx->skip_guard && (info.encoded_bytes > ctx->targets[t] || guard_size > GUARD_SIZE)) {
            *ctx->guard_overshoot = 1;
            fprintf(stderr, "[th%d t=%d] ENCODE OVERSHOOT %s: %dx%dx%d size=%d guard_size=%d\n", ctx->tid, ctx->targets[t], ctx->names[i], w, h, pimg->has_alpha ? 4 : 3, info.encoded_bytes, guard_size);
        }

        int dw = 0, dh = 0, dq = 0, dcomp = 0;
        unsigned char *dec = wtpc_decode_mem(enc, info.encoded_bytes, &dw, &dh, &dq, &dcomp);
        free(enc);
        if (!dec || dw != w || dh != h || dcomp < 3) {
            fprintf(stderr, "[th%d] BAD DECODE %s: %dx%dx%d (expected %dx%d)\n", ctx->tid, ctx->names[i], dw, dh, dcomp, w, h); continue;
        }

        char tmp_png[256];
        snprintf(tmp_png, sizeof(tmp_png), "/tmp/wtpc_tune_%d_%d_%d_%d.png", getpid(), ctx->tid, i, ctx->targets[t]);
        stbi_write_png(tmp_png, dw, dh, dcomp, dec, dw*dcomp);
        free(dec);

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", ctx->dir_path, ctx->names[i]);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "ssimulacra2 \"%s\" \"%s\" 2>/dev/null", path, tmp_png);
        FILE *fp = popen(cmd, "r");
        float score = 0, min_score = ctx->chroma_mode ? 48.f : 69.f;
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
        st->q_sum += info.result_q;
        int dev = abs(info.encoded_bytes - ctx->targets[t]);
        if (dev > 0 && info.encoded_bytes < ctx->targets[t] && info.result_q <= 1) dev = 0;
        st->dev_sum += dev;
        if (dev > st->max_dev) st->max_dev = dev;

        if (g_tune_verbose || (t == (ctx->ntargets - 1) && score < min_score && strcmp(ctx->names[i], "n01807496_partridge_256.png"))) {
            pthread_mutex_lock(ctx->print_mutex);
            fprintf(stderr, "[th%d t=%d] %s: ssim2=%11.6f n=%3d q=%3d dev=%4d avg_s=%11.6f mean_d=%.1f max_d=%4d (%.1f%%) - %s\n",
                ctx->tid, ctx->targets[t], ctx->chroma_mode ? "420" : "444", score, st->count, info.result_q, dev,
                st->count > 0 ? st->ssim_sum / st->count : 0.0, st->count > 0 ? st->dev_sum / st->count : 0.0,
                st->max_dev, (st->max_dev*100.f/ctx->targets[t]), ctx->names[i]);
            pthread_mutex_unlock(ctx->print_mutex);
        }
    }
    return NULL;
}

/* Weight multipliers per target: give more importance to small targets (800B..2KB, 50..80KB for big images),
   since we can borrow quality from large files (q=1) without visual loss. */
static const float tune_weights[] = {3.0f, 2.0f, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f};

/* --- Automated grid-search: sweeps one param at a time, keeps best, continues --- */
static void tune_grid(const char *dir_path, const ParamCfg *cfg, int nparams, int base_idx, int chroma_mode, int start_param, int big_images, const int *targets, int ntargets) {
    if (system("which ssimulacra2 >/dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: ssimulacra2 not found in PATH\n");
        return;
    }
    /* Init g_params from current g_quant tables (start from previous run's values) */
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_LH0 + b] = g_quant_y_lh[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_HL0 + b] = g_quant_y_hl[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_HH0 + b] = g_quant_y_hh[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_DZ_LH0 + b] = g_quant_y_lh[MAX_BANDS + b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_DZ_HL0 + b] = g_quant_y_hl[MAX_BANDS + b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_Y_DZ_HH0 + b] = g_quant_y_hh[MAX_BANDS + b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_LH0 + b] = g_quant_c_lh[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_HL0 + b] = g_quant_c_hl[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_HH0 + b] = g_quant_c_hh[b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_DZ_LH0 + b] = g_quant_c_lh[MAX_BANDS + b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_DZ_HL0 + b] = g_quant_c_hl[MAX_BANDS + b];
    for (int b = 0; b < MAX_BANDS; b++) g_params[P_C_DZ_HH0 + b] = g_quant_c_hh[MAX_BANDS + b];
    g_params[P_DC_Y] = g_quant_y_lh[MAX_BANDS*2];
    g_params[P_DC_C] = g_quant_c_lh[MAX_BANDS*2];
    g_params[P_EXT_MULT_Y] = g_quant_y_lh[MAX_BANDS*2+1];
    g_params[P_EXT_MULT_HH_Y] = g_quant_y_lh[MAX_BANDS*2+2];
    g_params[P_EXT_MULT_C] = g_quant_c_lh[MAX_BANDS*2+1];
    g_params[P_EXT_MULT_HH_C] = g_quant_c_lh[MAX_BANDS*2+2];
    if (chroma_mode) {
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_LH0 + b] = g_quant_c420_lh[b];
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_HL0 + b] = g_quant_c420_hl[b];
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_HH0 + b] = g_quant_c420_hh[b];
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_DZ_LH0 + b] = g_quant_c420_lh[MAX_BANDS + b];
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_DZ_HL0 + b] = g_quant_c420_hl[MAX_BANDS + b];
        for (int b = 0; b < MAX_BANDS; b++) g_params[P_C420_DZ_HH0 + b] = g_quant_c420_hh[MAX_BANDS + b];
        g_params[P_DC_C420] = g_quant_c420_lh[MAX_BANDS*2];
        g_params[P_EXT_MULT_C420] = g_quant_c420_lh[MAX_BANDS*2+1];
        g_params[P_EXT_MULT_HH_C420] = g_quant_c420_lh[MAX_BANDS*2+2];
    }
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
        int _guard_overshoot = 0; \
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
            _ctx[_tid].guard_overshoot = &_guard_overshoot; \
            _ctx[_tid].skip_guard    = big_images; \
            _ctx[_tid].stats       = _astats + _tid * ntargets; \
            memset(_ctx[_tid].stats, 0, ntargets * sizeof(PerTargetStats)); \
            pthread_create(&_th[_tid], NULL, tune_worker, &_ctx[_tid]); \
        } \
        for (int _tid = 0; _tid < _nth; _tid++) pthread_join(_th[_tid], NULL); \
        pthread_mutex_destroy(&_mutex); \
        pthread_mutex_destroy(&_pmutex); \
        if (_guard_overshoot) { \
            free(_astats); \
            fprintf(stderr, "  %-15s %s = %6.3f  GUARD OVERSHOOT (%dB unreachable), step FAILED\n", pcfg->name, label, (val), GUARD_SIZE); \
            *(ssim_out) = -9999.0f; \
            break; \
        } \
        /* Aggregate across threads, print per-target breakdown */ \
        double _sum_ssim2 = 0, _sum_w = 0; int _count = 0; \
        int _overall_max_dev = 0; \
        double _overall_max_dev_pct = 0; \
        double _overall_mean_dev_sum = 0; \
        double _overall_mean_pct_sum = 0; \
        for (int _tidx = 0; _tidx < ntargets; _tidx++) { \
            double _ts = 0, _td = 0, _tq = 0; int _tn = 0, _tm = 0; \
            for (int _tid = 0; _tid < _nth; _tid++) { \
                PerTargetStats *_st = &_astats[_tid * ntargets + _tidx]; \
                _ts += _st->ssim_sum; _tn += _st->count; \
                _td += _st->dev_sum; \
                _tq += _st->q_sum; \
                if (_st->max_dev > _tm) _tm = _st->max_dev; \
            } \
            if (_tn > 0) { \
                double _as = _ts / _tn; \
                double _ad = _td / _tn; \
                double _aq = _tq / _tn; \
                float _w = big_images ? 1.0f : tune_weights[_tidx]; \
                _sum_ssim2 += _as * _w; _sum_w += _w; _count++; \
                double _dp = _ad * 100.0 / targets[_tidx]; \
                double _mp = _tm * 100.0 / targets[_tidx]; \
                if (_tm > _overall_max_dev) { _overall_max_dev = _tm; _overall_max_dev_pct = _mp; } \
                _overall_mean_dev_sum += _ad; \
                _overall_mean_pct_sum += _dp; \
                fprintf(stderr, "[t=%d w=%.1f n=%d ssim2=%-9.6f mean_q=%.1f mean_d=%.1f (%.1f%%) max_d=%d (%.1f%%)]%s", targets[_tidx], _w, _tn, _as, _aq, _ad, _dp, _tm, _mp, _tidx == (ntargets - 1) ? "\n" : " "); \
            } \
        } \
        free(_astats); \
        double _avg = _sum_w > 0 ? _sum_ssim2 / _sum_w : 0; \
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
        if (best_ssim < -9000.0f) { fprintf(stderr, "  Parameter %s: center %.3f fails guard, skipping.\n", pcfg->name, center); continue; }

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
                if (val <= 0.0f || (strstr(pcfg->name, "dz") && val < 0.5f)) { neg_stop = 1; skipped_neg = max_d - d + 1; }
                else {
                    TUNE_EVAL(val, &ssim, " -");
                    if (ssim < -9000.0f) {
                        /* Guard overshoot: try trading with neighbour bands before giving up.
                           Borrow bits from adjacent bands by making their quantizers coarser. */
                        float saved_best_val = best_val;
                        float trade_best_ssim = -9999.0f, trade_best_other = 0;
                        int trade_ok = 0, trade_other_p = 0;

                        /* Step 1: next band +1..+3 (same channel) */
                        if ((p % 8) != 7) {
                            float nxt_center = g_params[base_idx + p + 1];
                            fprintf(stderr, "  Trying trade n+1: %s\n", cfg[p+1].name);
                            for (int td = 1; td <= 3 && !trade_ok; td++) {
                                float nxt_val = nxt_center + td * cfg[p + 1].delta;
                                fprintf(stderr, "  trade %s +%d = %.3f\n", cfg[p+1].name, td, nxt_val);
                                g_params[base_idx + p] = val;
                                g_params[base_idx + p + 1] = nxt_val;
                                apply_params();
                                float ts;
                                TUNE_EVAL(val, &ts, " G");
                                g_params[base_idx + p] = saved_best_val;
                                g_params[base_idx + p + 1] = nxt_center;
                                apply_params();
                                if (ts > best_ssim) {
                                    trade_best_ssim = ts; trade_best_other = nxt_val;
                                    trade_other_p = p + 1; trade_ok = 1;
                                }
                            }
                        }
                        /* Step 2: previous band +1..+3 (same channel) */
                        if (!trade_ok && (p % 8) != 0) {
                            float prv_center = g_params[base_idx + p - 1];
                            fprintf(stderr, "  Trying trade n-1: %s\n", cfg[p-1].name);
                            for (int td = 1; td <= 3 && !trade_ok; td++) {
                                float prv_val = prv_center + td * cfg[p - 1].delta;
                                fprintf(stderr, "  trade %s +%d = %.3f\n", cfg[p-1].name, td, prv_val);
                                g_params[base_idx + p] = val;
                                g_params[base_idx + p - 1] = prv_val;
                                apply_params();
                                float ts;
                                TUNE_EVAL(val, &ts, " G");
                                g_params[base_idx + p] = saved_best_val;
                                g_params[base_idx + p - 1] = prv_center;
                                apply_params();
                                if (ts > best_ssim) {
                                    trade_best_ssim = ts; trade_best_other = prv_val;
                                    trade_other_p = p - 1; trade_ok = 1;
                                }
                            }
                        }
                        /* Step 3: cross-channel (Y<>C), same subband type + band */
                        if (!trade_ok && chroma_mode == 0) {
                            int cross_p = -1;
                            if (p < 96) cross_p = (p < 48) ? p + 48 : p - 48;  /* Y<->C */
                            if (cross_p >= 0 && cross_p < nparams) {
                                float cross_center = g_params[base_idx + cross_p];
                                fprintf(stderr, "  Trying trade cross: %s\n", cfg[cross_p].name);
                                for (int td = 1; td <= 3 && !trade_ok; td++) {
                                    float cross_val = cross_center + td * cfg[cross_p].delta;
                                    fprintf(stderr, "  trade %s +%d = %.3f\n", cfg[cross_p].name, td, cross_val);
                                    g_params[base_idx + p] = val;
                                    g_params[base_idx + cross_p] = cross_val;
                                    apply_params();
                                    float ts;
                                    TUNE_EVAL(val, &ts, " G");
                                    g_params[base_idx + p] = saved_best_val;
                                    g_params[base_idx + cross_p] = cross_center;
                                    apply_params();
                                    if (ts > best_ssim) {
                                        trade_best_ssim = ts; trade_best_other = cross_val;
                                        trade_other_p = cross_p; trade_ok = 1;
                                    }
                                }
                            }
                        }

                        if (trade_ok) {
                            best_ssim = trade_best_ssim; best_val = val;
                            g_params[base_idx + trade_other_p] = trade_best_other;
                            apply_params();
                            neg_stop = 1; skipped_neg = max_d - d;
                            fprintf(stderr, "  Negative search: guard traded with %s = %.3f (ssim2=%.6f)\n",
                                cfg[trade_other_p].name, trade_best_other, trade_best_ssim);
                        } else {
                            neg_stop = 1; skipped_neg = max_d - d;
                            fprintf(stderr, "  Negative search stopped: guard overshoot at %.3f (no trade found)\n", val);
                        }
                    }
                    else if (ssim > best_ssim) { best_ssim = ssim; best_val = val; neg_down = 0; }
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

        fprintf(stderr, "\n>>> BEST %s = %.3f (ssim2 = %.6f)\n", pcfg->name, best_val, best_ssim);
        printf("# BEST_%s, %.3f, %.6f\n", pcfg->name, best_val, best_ssim);
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

/* =====================================================================
 *  qfit recalibration (-R)
 *  Re-derives the quadratic size(q) model used by find_quality_for_target
 *  for all 6 encoder combinations (ebcot/huffman/huffman-ctx x 444/420).
 * ===================================================================== */

/* One encoder configuration = a row/col of the qfit table. */
typedef struct { const char *name; int encode_mode; int huf_extra_ctx; int chroma_420; } CalibCfg;
static const CalibCfg calib_cfgs[6] = {
    {"EBCOT       4:4:4", WTPC_ENC_EBCOT, 0, 0},
    {"EBCOT       4:2:0", WTPC_ENC_EBCOT, 0, 1},
    {"Huffman1tbl 4:4:4", WTPC_ENC_HUFFMAN, 0, 0},
    {"Huffman1tbl 4:2:0", WTPC_ENC_HUFFMAN, 0, 1},
    {"Huffman ctx 4:4:4", WTPC_ENC_HUFFMAN, 1, 0},
    {"Huffman ctx 4:2:0", WTPC_ENC_HUFFMAN, 1, 1},
};

typedef struct {
    int tid;
    int *next_idx;
    pthread_mutex_t *mutex;
    const CalibCfg *cfg;
    PreloadedImg *images;
    int nimg;
    int *targets;
    int ntargets;
    int ntotal;
    /* outputs (per work item, indexed [t*nimg + i]) */
    float *out_lt;      /* ln(target_n) with npix normalization, or NAN if skipped */
    float *out_lb;      /* ln(base(result_q)) */
    int   *out_steps;   /* search_steps */
    int   *out_dev;     /* abs(encoded_bytes - target) */
} CalibCtx;

#define CALIB_Q_REF_NPIX 65536.0f  /* must match find_quality_for_target */

static void *calib_worker(void *arg) {
    CalibCtx *ctx = (CalibCtx*)arg;
    const CalibCfg *cfg = ctx->cfg;
    while (1) {
        pthread_mutex_lock(ctx->mutex);
        int idx = (*ctx->next_idx)++;
        pthread_mutex_unlock(ctx->mutex);
        if (idx >= ctx->ntotal) break;
        int t = idx / ctx->nimg, i = idx % ctx->nimg;
        ctx->out_lt[idx] = NAN; ctx->out_lb[idx] = NAN; ctx->out_steps[idx] = 0; ctx->out_dev[idx] = -1;
        PreloadedImg *pimg = &ctx->images[i];
        if (!pimg->rgb || pimg->w <= 0 || pimg->h <= 0) continue;
        wtpc_enc_info info;
        unsigned char *enc = wtpc_encode_mem(pimg->rgb, &info, pimg->w, pimg->h,
            ctx->targets[t], 0, cfg->chroma_420, cfg->encode_mode, cfg->huf_extra_ctx, pimg->has_alpha, 0, 0);
        if (!enc) continue;
        free(enc);
        int dev = abs(info.encoded_bytes - ctx->targets[t]);
        if (dev > 0 && info.encoded_bytes < ctx->targets[t] && info.result_q <= 1) dev = 0;
        ctx->out_dev[idx] = dev;
        if (info.result_q < 1) { ctx->out_steps[idx] = info.search_steps; continue; }
        float npix = (float)(pimg->w * pimg->h);
        float target_n = (float)ctx->targets[t] * powf(CALIB_Q_REF_NPIX / npix, 0.35f);
        if (target_n < 1.0f) target_n = 1.0f;
        ctx->out_lt[idx] = logf(target_n);
        ctx->out_lb[idx] = logf(compute_base(info.result_q));
        ctx->out_steps[idx] = info.search_steps;
    }
    return NULL;
}

/* Solve 3x3 normal equations (least squares quadratic) via Gaussian */
/* elimination with partial pivoting. Fills out[3] = {c2, c1, c0}.   */
static int solve_quad_lsq(const float *lt, const float *lb, int n, float out[3]) {
    double A[3][4] = {{0}};
    int used = 0;
    for (int k = 0; k < n; k++) {
        if (isnan(lt[k]) || isnan(lb[k])) continue;
        double x = lt[k], y = lb[k];
        double b[3] = { x*x, x, 1.0 };
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) A[r][c] += b[r]*b[c];
            A[r][3] += b[r]*y;
        }
        used++;
    }
    if (used < 3) return 0;
    for (int col = 0; col < 3; col++) {
        int piv = col;
        for (int r = col+1; r < 3; r++) if (fabs(A[r][col]) > fabs(A[piv][col])) piv = r;
        if (fabs(A[piv][col]) < 1e-12) return 0;
        if (piv != col) for (int c = 0; c < 4; c++) { double tmp = A[col][c]; A[col][c] = A[piv][c]; A[piv][c] = tmp; }
        for (int r = 0; r < 3; r++) {
            if (r == col) continue;
            double f = A[r][col] / A[col][col];
            for (int c = col; c < 4; c++) A[r][c] -= f * A[col][c];
        }
    }
    out[0] = (float)(A[0][3] / A[0][0]);
    out[1] = (float)(A[1][3] / A[1][1]);
    out[2] = (float)(A[2][3] / A[2][2]);
    return 1;
}

static void calibrate_qfit(const char *dir_path) {
    int targets[] = {200, 400, 800, 1000, 2000, 4000, 8000, 16000, 26000, 36000};
    int ntargets = sizeof(targets) / sizeof(targets[0]);

    /* Collect + preload images (same as tune_grid). */
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
    PreloadedImg *images = malloc(nimg * sizeof(PreloadedImg));
    if (!images) { fprintf(stderr, "OOM\n"); return; }
    for (int i = 0; i < nimg; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, names[i]);
        const char *ext = strrchr(names[i], '.');
        if (ext && strcmp(ext, ".png") == 0) images[i].rgb = read_png(path, &images[i].w, &images[i].h, &images[i].has_alpha);
        else { int comp; images[i].rgb = stbi_load(path, &images[i].w, &images[i].h, &comp, 0); images[i].has_alpha = (comp == 4); }
        if (!images[i].rgb) { images[i].w = images[i].h = 0; images[i].has_alpha = 0; }
    }
    fprintf(stderr, "Preloaded %d images\n", nimg);

    int ntotal = nimg * ntargets;
    float *lt = malloc(ntotal * sizeof(float));
    float *lb = malloc(ntotal * sizeof(float));
    int   *stp = malloc(ntotal * sizeof(int));
    int   *dev = malloc(ntotal * sizeof(int));
    float fits[6][3];

    printf("    static const float qfit[3][2][3] = {\n");
    double t_start = now_ms();
    for (int cc = 0; cc < 6; cc++) {
        const CalibCfg *cfg = &calib_cfgs[cc];
        int next_idx = 0;
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        int nth = ntotal < TUNE_THREADS ? ntotal : TUNE_THREADS;
        CalibCtx cx[TUNE_THREADS]; pthread_t th[TUNE_THREADS];
        for (int k = 0; k < nth; k++) {
            cx[k].tid = k; cx[k].next_idx = &next_idx; cx[k].mutex = &mutex; cx[k].cfg = cfg;
            cx[k].images = images; cx[k].nimg = nimg; cx[k].targets = targets; cx[k].ntargets = ntargets;
            cx[k].ntotal = ntotal; cx[k].out_lt = lt; cx[k].out_lb = lb; cx[k].out_steps = stp; cx[k].out_dev = dev;
            pthread_create(&th[k], NULL, calib_worker, &cx[k]);
        }
        for (int k = 0; k < nth; k++) pthread_join(th[k], NULL);
        pthread_mutex_destroy(&mutex);

        /* Fit quadratic over the whole (lt, lb) cloud. */
        int ok = solve_quad_lsq(lt, lb, ntotal, fits[cc]);
        if (!ok) { fprintf(stderr, "[%s] fit FAILED (too few points)\n", cfg->name); fits[cc][0]=fits[cc][1]=fits[cc][2]=0; }

        /* Emit the table row (grouped 3 rows of 2 cols: coder x chroma). */
        int is444 = !cfg->chroma_420;
        if (is444) printf("        /* %-12s */ { { %9.5ff, %9.5ff, %9.5ff },   /* 4:4:4 */\n",
                          cfg->encode_mode == WTPC_ENC_EBCOT ? "EBCOT" : (cfg->huf_extra_ctx ? "Huffman ctx" : "Huffman 1tbl"),
                          fits[cc][0], fits[cc][1], fits[cc][2]);
        else       printf("                            { %9.5ff, %9.5ff, %9.5ff } }, /* 4:2:0 */\n",
                          fits[cc][0], fits[cc][1], fits[cc][2]);

        /* Per-target + overall step / deviation stats. */
        int overall_max = 0; double overall_sum = 0; int overall_n = 0;
        int overall_maxd = 0; double overall_devsum = 0; int overall_devn = 0;
        fprintf(stderr, "--- %s ---\n", cfg->name);
        for (int t = 0; t < ntargets; t++) {
            int tmax = 0, tn = 0; double tsum = 0;
            int tmaxd = 0, tdn = 0; double tdsum = 0;
            for (int i = 0; i < nimg; i++) {
                int s = stp[t*nimg + i];
                if (s > 0) {
                    tsum += s; tn++; if (s > tmax) tmax = s;
                    overall_sum += s; overall_n++; if (s > overall_max) overall_max = s;
                }
                int dv = dev[t*nimg + i];
                if (dv >= 0) {
                    tdsum += dv; tdn++; if (dv > tmaxd) tmaxd = dv;
                    overall_devsum += dv; overall_devn++; if (dv > overall_maxd) overall_maxd = dv;
                }
            }
            fprintf(stderr, "  t=%-6d mean_steps=%.2f max_steps=%d  mean_dev=%.1f max_dev=%d\n",
                    targets[t], tn ? tsum/tn : 0.0, tmax, tdn ? tdsum/tdn : 0.0, tmaxd);
        }
        fprintf(stderr, "  OVERALL mean_steps=%.2f max_steps=%d  mean_dev=%.1f max_dev=%d\n",
                overall_n ? overall_sum/overall_n : 0.0, overall_max,
                overall_devn ? overall_devsum/overall_devn : 0.0, overall_maxd);
    }
    printf("    };\n");
    fflush(stdout);
    fprintf(stderr, "\nCalibration done in %.1f min\n", (now_ms() - t_start) * 0.001 / 60.0);

    free(lt); free(lb); free(stp); free(dev);
    for (int i = 0; i < nimg; i++) free(names[i]);
    free(names);
    for (int i = 0; i < nimg; i++) if (images[i].rgb) stbi_image_free(images[i].rgb);
    free(images);
}
#endif  /* WTPC_TUNE_PARAMS */
#ifdef WTPC_TUNE_CTX
/* Train priors per-quality: encode all images at each training Q, accumulate
   per-context stats, and print separate priors tables for each quality level. */
static void train_priors(const char *dir_path) {
    int nq = sizeof(TRAIN_Q) / sizeof(TRAIN_Q[0]);
    /* Collect + preload images (same as calibrate_qfit). */
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

    fprintf(stderr, "Training priors on %d images x %d qualities = %d encodes...\n", nimg, nq, nimg * nq);

    int overflow = 0;  /* track uint8_t overflow warnings */
    unsigned long long grand_total_bytes = 0;
    double tscale_e = 0.25;  /* default T-scaling exponent */
    char *env_e = getenv("WTPC_E");
    if (env_e) tscale_e = atof(env_e);

    /* For each training quality level */
    for (int qi = 0; qi < nq; qi++) {
        int q = TRAIN_Q[qi];
        reset_ctx_stats();
        unsigned long long total_bytes = 0;

        int done = 0;
        for (int i = 0; i < nimg; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir_path, names[i]);
            int w, h, has_alpha;
            const char *ext = strrchr(names[i], '.');
            unsigned char *rgb = NULL;
            if (ext && strcmp(ext, ".png") == 0)
                rgb = read_png(path, &w, &h, &has_alpha);
            else {
                int comp;
                rgb = stbi_load(path, &w, &h, &comp, 4);
                has_alpha = (comp == 4);
            }
            if (!rgb || w <= 0 || h <= 0) { if (rgb) stbi_image_free(rgb); continue; }

            wtpc_enc_info info = {0};
            unsigned char *enc = wtpc_encode_mem(rgb, &info, w, h, 0, q, 0, 2, 0, has_alpha, 0, 0);
            if (enc) { total_bytes += info.encoded_bytes; free(enc); }

            if (ext && strcmp(ext, ".png") == 0) free(rgb); else stbi_image_free(rgb);
            done++;
            if (done % 100 == 0 || done == nimg)
                fprintf(stderr, "  q=%d: %d/%d images done\n", q, done, nimg);
        }
        grand_total_bytes += total_bytes;
        fprintf(stderr, "  TOTAL_BYTES q=%d: %llu\n", q, total_bytes);

        /* Output priors for this quality */
        printf("/* Q=%d */\n", q);

        /* Significance priors */
        printf("static const uint8_t priors_sig_q%d[CTX_SIG][2] = {\n", q);
        unsigned long long total_all[CTX_SIG]; unsigned long long avg_total = 0;
        int nctx = 0;
        for (int i = 0; i < CTX_SIG; i++) {
            total_all[i] = g_sig_cnt0[i] + g_sig_cnt1[i];
            if (total_all[i] > 0) { avg_total += total_all[i]; nctx++; }
        }
        if (nctx > 0) avg_total /= nctx;
        for (int group = 0; group < 36; group++) {
            printf("    ");
            for (int j = 0; j < 9; j++) {
                int ctx = group * 9 + j;
                unsigned long long c0 = g_sig_cnt0[ctx], c1 = g_sig_cnt1[ctx];
                unsigned long long t = c0 + c1;
                int pc0 = 1, pc1 = 1;
                if (t > 0 && avg_total > 0) {
                    double p0 = (double)c0 / (double)t;
                    double ratio = (double)avg_total / (double)t;
                    int T = (int)(16.0 * pow(ratio, tscale_e) + 0.5);
                    if (T < 2) T = 2;
                    if (T > 256) T = 256;
                    pc0 = (int)(p0 * T + 0.5);
                    if (pc0 < 1) pc0 = 1;
                    if (pc0 > T - 1) pc0 = T - 1;
                    pc1 = T - pc0;
                    if (j == 8 && p0 >= 0.45 && p0 <= 0.55) { pc0 = 1; pc1 = 1; }
                }
                if (pc0 > 255 || pc1 > 255) overflow = 1;
                printf("{%d,%d}%s", pc0, pc1, j < 8 ? "," : "");
            }
            printf("%s\n", group < 35 ? "," : "");
        }
        printf("};\n");

        /* Sign priors */
        printf("static const uint8_t priors_sgn_q%d[CTX_SGN][2] = {\n", q);
        { unsigned long long total[CTX_SGN]; unsigned long long avg = 0; int nz = 0;
          for (int i = 0; i < CTX_SGN; i++) {
              total[i] = g_sgn_cnt0[i] + g_sgn_cnt1[i];
              if (total[i] > 0) { avg += total[i]; nz++; }
          }
          if (nz > 0) avg /= nz;
          printf("    ");
          for (int i = 0; i < CTX_SGN; i++) {
              unsigned long long c0 = g_sgn_cnt0[i], c1 = g_sgn_cnt1[i], t = c0 + c1;
              int pc0 = 1, pc1 = 1;
              if (t > 0 && avg > 0) {
                  double p0 = (double)c0 / (double)t;
                  double ratio = (double)avg / (double)t;
                  int T = (int)(16.0 * pow(ratio, tscale_e) + 0.5);
                  if (T < 2) T = 2;
                  if (T > 256) T = 256;
                  pc0 = (int)(p0 * T + 0.5);
                  if (pc0 < 1) pc0 = 1;
                  if (pc0 > T - 1) pc0 = T - 1;
                  pc1 = T - pc0;
              }
              if (pc0 > 255 || pc1 > 255) overflow = 1;
              printf("{%d,%d}%s", pc0, pc1, i < CTX_SGN - 1 ? "," : "");
          }
          printf("\n");
        }
        printf("};\n");

        /* Refinement priors */
        printf("static const uint8_t priors_ref_q%d[CTX_REF][2] = {\n", q);
        { unsigned long long total[CTX_REF]; unsigned long long avg = 0; int nz = 0;
          for (int i = 0; i < CTX_REF; i++) {
              total[i] = g_ref_cnt0[i] + g_ref_cnt1[i];
              if (total[i] > 0) { avg += total[i]; nz++; }
          }
          if (nz > 0) avg /= nz;
          printf("    ");
          for (int i = 0; i < CTX_REF; i++) {
              unsigned long long c0 = g_ref_cnt0[i], c1 = g_ref_cnt1[i], t = c0 + c1;
              int pc0 = 1, pc1 = 1;
              if (t > 0 && avg > 0) {
                  double p0 = (double)c0 / (double)t;
                  double ratio = (double)avg / (double)t;
                  int T = (int)(16.0 * pow(ratio, tscale_e) + 0.5);
                  if (T < 2) T = 2;
                  if (T > 256) T = 256;
                  pc0 = (int)(p0 * T + 0.5);
                  if (pc0 < 1) pc0 = 1;
                  if (pc0 > T - 1) pc0 = T - 1;
                  pc1 = T - pc0;
              }
              if (pc0 > 255 || pc1 > 255) overflow = 1;
              printf("{%d,%d}%s", pc0, pc1, i < CTX_REF - 1 ? "," : "");
          }
          printf("\n");
        }
        printf("};\n");

        /* BP + flag priors */
        printf("static const uint8_t priors_bp_q%d[CTX_BP][2] = {\n", q);
        { unsigned long long total[CTX_BP]; unsigned long long avg = 0; int nz = 0;
          for (int i = 0; i < CTX_BP; i++) {
              total[i] = g_bp_cnt0[i] + g_bp_cnt1[i];
              if (total[i] > 0) { avg += total[i]; nz++; }
          }
          if (nz > 0) avg /= nz;
          printf("    ");
          for (int i = 0; i < CTX_BP; i++) {
              unsigned long long c0 = g_bp_cnt0[i], c1 = g_bp_cnt1[i], t = c0 + c1;
              int pc0 = 1, pc1 = 1;
              if (t > 0 && avg > 0) {
                  double p0 = (double)c0 / (double)t;
                  double ratio = (double)avg / (double)t;
                  int T = (int)(16.0 * pow(ratio, tscale_e) + 0.5);
                  if (T < 2) T = 2;
                  if (T > 256) T = 256;
                  pc0 = (int)(p0 * T + 0.5);
                  if (pc0 < 1) pc0 = 1;
                  if (pc0 > T - 1) pc0 = T - 1;
                  pc1 = T - pc0;
              }
              if (pc0 > 255 || pc1 > 255) overflow = 1;
              printf("{%d,%d}%s", pc0, pc1, i < CTX_BP - 1 ? "," : "");
          }
          printf("\n");
        }
        printf("};\n");

        /* DC priors: 4 len + 1 sign + 15 magnitude = 20 contexts */
        printf("static const uint8_t priors_dc_q%d[CTX_DC][2] = {\n", q);
        { unsigned long long total[CTX_DC]; unsigned long long avg = 0; int nz = 0;
          for (int i = 0; i < CTX_DC; i++) {
              total[i] = g_dc_cnt0[i] + g_dc_cnt1[i];
              if (total[i] > 0) { avg += total[i]; nz++; }
          }
          if (nz > 0) avg /= nz;
          printf("    ");
          for (int i = 0; i < CTX_DC; i++) {
              unsigned long long c0 = g_dc_cnt0[i], c1 = g_dc_cnt1[i], t = c0 + c1;
              int pc0 = 1, pc1 = 1;
              if (t > 0 && avg > 0) {
                  double p0 = (double)c0 / (double)t;
                  double ratio = (double)avg / (double)t;
                  int T = (int)(16.0 * pow(ratio, tscale_e) + 0.5);
                  if (T < 2) T = 2;
                  if (T > 256) T = 256;
                  pc0 = (int)(p0 * T + 0.5);
                  if (pc0 < 1) pc0 = 1;
                  if (pc0 > T - 1) pc0 = T - 1;
                  pc1 = T - pc0;
              }
              if (pc0 > 255 || pc1 > 255) overflow = 1;
              printf("{%d,%d}%s", pc0, pc1, i < CTX_DC - 1 ? "," : "");
          }
          printf("\n");
        }
        printf("};\n\n");
        fflush(stdout);
    }

    /* Output pointer arrays */
    printf("static const uint16_t (*const priors_sig_q[%d])[2] = {\n", nq);
    printf("    ");
    for (int qi = 0; qi < nq; qi++)
        printf("priors_sig_q%d%s", TRAIN_Q[qi], qi < nq-1 ? ", " : "");
    printf("\n};\n");
    printf("static const uint16_t (*const priors_sgn_q[%d])[2] = {\n", nq);
    printf("    ");
    for (int qi = 0; qi < nq; qi++)
        printf("priors_sgn_q%d%s", TRAIN_Q[qi], qi < nq-1 ? ", " : "");
    printf("\n};\n");
    printf("static const uint16_t (*const priors_ref_q[%d])[2] = {\n", nq);
    printf("    ");
    for (int qi = 0; qi < nq; qi++)
        printf("priors_ref_q%d%s", TRAIN_Q[qi], qi < nq-1 ? ", " : "");
    printf("\n};\n");
    printf("static const uint16_t (*const priors_bp_q[%d])[2] = {\n", nq);
    printf("    ");
    for (int qi = 0; qi < nq; qi++)
        printf("priors_bp_q%d%s", TRAIN_Q[qi], qi < nq-1 ? ", " : "");
    printf("\n};\n");
    printf("static const uint16_t (*const priors_dc_q[%d])[2] = {\n", nq);
    printf("    ");
    for (int qi = 0; qi < nq; qi++)
        printf("priors_dc_q%d%s", TRAIN_Q[qi], qi < nq-1 ? ", " : "");
    printf("\n};\n");


    for (int i = 0; i < nimg; i++) free(names[i]);
    free(names);
    if (overflow) fprintf(stderr, "WARNING: some priors exceed 255, uint8_t will overflow! Increase to uint16_t.\n");
    fprintf(stderr, "GRAND_TOTAL_BYTES: %llu\n", grand_total_bytes);
    fprintf(stderr, "Training done (%d images)\n", nimg);
}
#endif  /* WTPC_TUNE_CTX */

int main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int mode = -1;  /* 0=decode, 1=encode, 2=test */
    int quality = 20;
    int chroma_420 = 0;
    int encode_mode = WTPC_ENC_EBCOT;  /* WTPC_ENC_AUTO/HUFFMAN/EBCOT, default EBCOT */
    int target_bytes = 0;  /* 0 means use -q directly */
    int block_size = 0;
    int huf_extra_ctx = 0; /* 0=single table, faster, 1 - context-aware (2 Huffman tables) for better compression, slower */
    int hash_mode = 0;     /* set by -m whash */
#ifdef WTPC_TUNE_PARAMS
    int tune_start = 0;  /* -S: start grid from this param index (0=first) */
    int tune_420   = 0;  /* -420: tune chroma 4:2:0 params instead of main */
    int tune_ext   = 0;  /* -E: tune EXT multipliers (large-image dataset) */
    { const char *ev = getenv("WTPC_EXT_BASE_THRESH"); if (ev) g_ext_base_thresh = (float)atof(ev); }
#endif
#ifdef WTPC_TUNE_CDF97_D
    { const char *ev = getenv("WTPC_CDF97_D"); if (ev) g_cdf97_d = (float)atof(ev); }
#endif
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) mode = 1;
        else if (strcmp(argv[i], "-d") == 0) mode = 0;
        else if (strcmp(argv[i], "-t") == 0) mode = 2;
        else if (strcmp(argv[i], "-q") == 0 && i+1 < argc) quality = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) target_bytes = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0) chroma_420 = 1;
        else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) { const char *m = argv[++i]; if (strcmp(m, "huffman") == 0) encode_mode = WTPC_ENC_HUFFMAN; else if (strcmp(m, "ebcot") == 0) encode_mode = WTPC_ENC_EBCOT; else if (strcmp(m, "best") == 0) encode_mode = WTPC_ENC_AUTO; else if (strcmp(m, "whash") == 0) hash_mode = 1; else { printf("Unknown -m mode: %s\n", m); return 1; } }
        else if (strcmp(argv[i], "-h") == 0 && i+1 < argc) huf_extra_ctx = atoi(argv[++i]);
        else if (strcmp(argv[i], "--block") == 0 && i+1 < argc) block_size = atoi(argv[++i]);
#ifndef _WIN32
        else if (strcmp(argv[i], "-G") == 0) mode = 3;
#endif
#ifdef WTPC_TUNE_CTX
        else if (strcmp(argv[i], "-P") == 0) mode = 6;
#endif
#ifdef WTPC_TUNE_PARAMS
        else if (strcmp(argv[i], "-T") == 0) mode = 4;
        else if (strcmp(argv[i], "-R") == 0) mode = 5;
        else if (strcmp(argv[i], "-S") == 0 && i+1 < argc) tune_start = atoi(argv[++i]);
        else if (strcmp(argv[i], "-v") == 0) g_tune_verbose = 1;
        else if (strcmp(argv[i], "-420") == 0) tune_420 = 1;
        else if (strcmp(argv[i], "-E") == 0) tune_ext = 1;
#endif
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) output = argv[++i];
        else if (argv[i][0] != '-') input = argv[i];
    }

    if (mode == -1 || !input) {
        printf("Usage:\n");
        printf("  wtpc -e input.png -o output.wtp [-q QUALITY] [-c] [-b TARGET_BYTES]\n");
        printf("  wtpc -d input.wtp -o output.png\n");
        printf("  wtpc -t input.png [-q quality]   (self-test)\n");
        printf("  wtpc -e input.png -o output -m whash   (encode wavelet hash -> .whash)\n");
        printf("  wtpc -d input.whash -o output.png -m whash   (decode wavelet hash -> .png)\n");
        printf("  -e  encode mode\n");
        printf("  -d  decode mode\n");
        printf("  -q  quality 1 - %d (default 20)\n", MAX_QUALITY);
        printf("  -b  target size in bytes (auto-finds q, overrides -q)\n");
        printf("  -c  4:2:0 chroma subsampling\n");
        printf("  -m  best|ebcot|huffman|whash  encoding mode (default: ebcot)\n");
        printf("  -h  context-aware Huffman tables (default: 0 = single table - faster, 1 - slower and slightly better compression ~35kb->36kb)\n");
        printf("  --block N  EBCOT mode only: split bit planes into NxN blocks for fast large-image encoding (32/64/128/256/512/1024/2048)\n");
#ifndef _WIN32
        printf("  -G  generate huffman tables from images in directory\n");
#endif
#ifdef WTPC_TUNE_CTX
        printf("  -P  train significance priors on a directory of images\n");
#endif
#ifdef WTPC_TUNE_PARAMS
        printf("  -T  grid-search multipliers for tuning\n");
        printf("  -R  recalibrate qfit rate-control model (prints qfit table + step stats)\n");
        printf("  -420  tune chroma 4:2:0 params (default: main luma+chroma444+DZ)\n");
        printf("  -E    tune EXT multipliers (bands 8+, large-image dataset)\n");
        printf("  -S  start grid from param index (0..%d, default 0)\n", (tune_ext ? (tune_420 ? NPARAMS_EXT - 1 : 3) : (tune_420 ? NPARAMS_420 - 1 : NPARAMS_MAIN - 1)));
        printf("  -v  verbose: per-encode progress logging\n");
#endif
        printf("  -o  output file\n");
        return 1;
    }

    if (mode == 1) {
        if (hash_mode) {
            /* Wavelet hash encode: PNG -> .whash */
            char auto_out[1024];
            if (!output) {
                const char *dot = strrchr(input, '.');
                if (dot) { int len = (int)(dot - input); snprintf(auto_out, sizeof(auto_out), "%.*s.whash", len, input); }
                else snprintf(auto_out, sizeof(auto_out), "%s.whash", input);
                output = auto_out;
            } else {
                /* Append .whash if not already present */
                const char *oext = strrchr(output, '.');
                if (!oext || strcmp(oext, ".whash") != 0) {
                    snprintf(auto_out, sizeof(auto_out), "%s.whash", output);
                    output = auto_out;
                }
            }
            int w, h, comp;
            unsigned char *img = stbi_load(input, &w, &h, &comp, 3);
            if (!img) { printf("Cannot load: %s, reason: %s\n", input, stbi_failure_reason()); return 1; }
            if (w > 256 || h > 256) { printf("Hash mode supports max 256x256 (got %dx%d)\n", w, h); stbi_image_free(img); return 1; }
            int hlen;
            uint8_t *hash = wtpc_hash_encode_mem(img, w, h, &hlen);
            stbi_image_free(img);
            if (!hash) { printf("Hash encoding failed!\n"); return 1; }
            FILE *f = fopen(output, "wb");
            if (!f) { printf("Cannot write: %s\n", output); free(hash); return 1; }
            fwrite(hash, 1, (size_t)hlen, f);
            fclose(f);
            printf("Hash: %dx%d -> %d bytes -> %s\n", w, h, hlen, output);
            free(hash);
        } else {
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
#ifndef _WIN32    
            const char *ext = strrchr(input, '.');
            if (ext && strcmp(ext, ".png") == 0) {
                img = read_png(input, &w, &h, &has_alpha);
                if (!img) { printf("Cannot load: %s\n", input); return 1; }
            } else
#endif
            {
                img = stbi_load(input, &w, &h, &comp, 0);
                if (!img) { printf("Cannot load: %s, reason: %s\n", input, stbi_failure_reason()); return 1; }
                has_alpha = (comp == 4);
            }
        }
        /* Validate --block: must be 0 or power of 2 in [32..2048], EBCOT-only */
        if (block_size > 0) {
            int valid = 0;
            for (int s = 32; s <= 2048; s <<= 1) if (s == block_size) { valid = 1; break; }
            if (!valid) { printf("Invalid --block %d: must be 32,64,128,256,512,1024,2048\n", block_size); return 1; }
            if (encode_mode == WTPC_ENC_HUFFMAN) { printf("--block requires ebcot mode, not huffman\n"); return 1; }
        }
        double t0 = now_ms();
        int ret = wtpc_encode_file(output, img, &info, w, h, target_bytes, quality, chroma_420, encode_mode, huf_extra_ctx, has_alpha, 0, block_size);
        double dt = now_ms() - t0;
        stbi_image_free(img);
        if (ret != 0) {
            printf("Encoding failed!\n"); return 1;
        }
        printf("Encoded %s -> %s (q=%d mode=%s%s result_mode=%s steps=%d alpha=%s) in %.3f ms\n", input, output, info.result_q, encode_mode == WTPC_ENC_HUFFMAN ? "huffman" : encode_mode == WTPC_ENC_EBCOT ? "ebcot" : "best", chroma_420 ? " 420":"", info.ebcot ? "ebcot" : "huffman", info.search_steps, has_alpha ? (info.alpha_one ? "skip" : "encoded") : "none", dt);
        if (!info.ebcot)
            printf("  huffman  Y:tbl=%d(%s) U:tbl=%d(%s) V:tbl=%d(%s)  custom_bits=Y:%d U:%d V:%d\n",
                info.huffman_y_table, info.huffman_y_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_u_table, info.huffman_u_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_v_table, info.huffman_v_table == NUM_DEF_TABLES ? "custom" : "static",
                info.huffman_y_size, info.huffman_u_size, info.huffman_v_size);
        }
    } else if (mode == 0) {
        /* Auto-detect whash vs wtpc from extension or -m flag */
        const char *iext = strrchr(input, '.');
        int is_whash = hash_mode || (iext && strcmp(iext, ".whash") == 0);
        if (is_whash) {
            /* Wavelet hash decode: .whash -> .png */
            char auto_out[1024];
            if (!output) { snprintf(auto_out, sizeof(auto_out), "%s.png", input); output = auto_out; }
            FILE *f = fopen(input, "rb");
            if (!f) { printf("Cannot open: %s\n", input); return 1; }
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz < 3 || sz > 4096) { fclose(f); printf("Invalid whash file: %s (%ld bytes)\n", input, sz); return 1; }
            uint8_t *hash = (uint8_t*)malloc((size_t)sz);
            if (!hash || fread(hash, 1, (size_t)sz, f) != (size_t)sz) { free(hash); fclose(f); return 1; }
            fclose(f);
            int dec_w, dec_h;
            unsigned char *rgb = wtpc_hash_decode_mem(hash, (int)sz, &dec_w, &dec_h);
            free(hash);
            if (!rgb) { printf("Hash decoding failed!\n"); return 1; }
            if (!stbi_write_png(output, dec_w, dec_h, 3, rgb, dec_w*3)) { free(rgb); printf("Cannot write: %s\n", output); return 1; }
            free(rgb);
            printf("Hash: %dx%d <- %ld bytes -> %s\n", dec_w, dec_h, sz, output);
        } else {
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
        }
#ifndef _WIN32
    } else if (mode == 3) {
        if (!input) { printf("Usage: wtpc -g <directory>\n"); return 1; }
        generate_tables(input);
#endif
#ifdef WTPC_TUNE_PARAMS
    } else if (mode == 4) {
        if (!input) { printf("Usage: wtpc -T <directory> [-420] [-E] [-S <start>] [-v]\n"); return 1; }
        int tune_targets[] = {1000, 2000, 4000, 8000, 16000, 26000, 36000};
        int tune_targets_ext[] = {50*1024, 80*1024, 130*1024, 200*1024, 540*1024, 1000*1024, 1500*1024};
        int ntargets = sizeof(tune_targets) / sizeof(tune_targets[0]);
        if (tune_420 && !tune_ext) {
            /* -T -420: band C420 tuning */
            int nparams = NPARAMS_420;
            if (tune_start < 0 || tune_start >= nparams) { printf("Invalid -S: must be 0..%d\n", nparams-1); return 1; }
            tune_grid(input, param_cfg_420, NPARAMS_420, P_C420_LH0, 1, tune_start, 0, tune_targets, ntargets);
        } else if (tune_ext) {
            if (tune_420) {
                /* -T -E -420: EXT 4:2:0 (params 4..5, auto start=4) */
                int start = tune_start >= 0 ? tune_start : 4;
                if (start < 4 || start >= NPARAMS_EXT) { printf("Invalid -S: must be 4..%d\n", NPARAMS_EXT-1); return 1; }
                tune_grid(input, param_cfg_ext, NPARAMS_EXT, P_EXT_MULT_C420 - 4, 1, start, 1, tune_targets_ext, (int)(sizeof(tune_targets_ext)/sizeof(tune_targets_ext[0])));
            } else {
                /* -T -E: EXT 4:4:4 (params 0..3) */
                int nparams = 4;
                if (tune_start < 0 || tune_start >= nparams) { printf("Invalid -S: must be 0..%d\n", nparams-1); return 1; }
                tune_grid(input, param_cfg_ext, nparams, P_EXT_MULT_Y, 0, tune_start, 1, tune_targets_ext, (int)(sizeof(tune_targets_ext)/sizeof(tune_targets_ext[0])));
            }
        } else {
            /* -T: main band tuning */
            int nparams = NPARAMS_MAIN;
            if (tune_start < 0 || tune_start >= nparams) { printf("Invalid -S: must be 0..%d\n", nparams-1); return 1; }
            tune_grid(input, param_cfg, NPARAMS_MAIN, 0, 0, tune_start, 0, tune_targets, ntargets);
        }
    } else if (mode == 5) {
        if (!input) { printf("Usage: wtpc -R <directory>\n"); return 1; }
        calibrate_qfit(input);
#endif
#ifdef WTPC_TUNE_CTX
    } else if (mode == 6) {
        if (!input) { printf("Usage: wtpc -P <directory>\n"); return 1; }
        train_priors(input);
#endif
    } else {
        /* Self-test: Huffman encode/decode roundtrip */
        int all_ok = 1;
        int16_t test_coeffs[1000];
        for(int i = 0; i < 1000; i++) test_coeffs[i] = 0;
        test_coeffs[0] = 5; test_coeffs[10] = -3; test_coeffs[50] = 120; test_coeffs[999] = 7;
        int t_pw = 1000+2; int16_t t_pad[1002*3] = {0};
        for(int x=0;x<1000;x++) t_pad[1*t_pw+(x+1)]=test_coeffs[x];
        int freq[NUM_HUFF_SYMBOLS];
        count_frequencies(t_pad, 1000, 1, freq);
        int cl[NUM_HUFF_SYMBOLS];
        uint32_t hc[NUM_HUFF_SYMBOLS];
        build_huffman_codes(freq, NUM_HUFF_SYMBOLS, cl);
        generate_canonical_codes(cl, NUM_HUFF_SYMBOLS, hc);
        uint8_t *enc_buf=(uint8_t*)malloc(10000);
        Bitstream bs; bitstream_init(&bs, enc_buf, 10000);
        huffman_encode_runval(&bs, t_pad, 1000, 1, hc, cl);
        bitstream_flush(&bs);
        int enc_len = bitstream_bytes(&bs);
        int16_t *dec_coeffs = (int16_t*)calloc((size_t)(1000+2)*3, sizeof(int16_t));
        Bitstream rbs;
        bitstream_init(&rbs, enc_buf, enc_len);
        huffman_decode_channel(&rbs, dec_coeffs, 1000, 1, cl, hc);
        int errors = 0;
        for(int y = 0; y < 1; y++)
            for(int x = 0; x < 1000; x++)
                if(test_coeffs[y*1000 + x] != dec_coeffs[(size_t)(y+1)*1002 + (x+1)]) { if(errors<10) printf("[%d] %d!=%d\n", y*1000+x, test_coeffs[y*1000+x], (int)dec_coeffs[(size_t)(y+1)*1002 + (x+1)]); errors++; }
        if(errors == 0)
            printf("[SELF-TEST HUFFMAN] OK: 1000 coeffs, %d bytes\n", enc_len);
        else
            { printf("[SELF-TEST HUFFMAN] FAIL: %d mismatches\n", errors); all_ok = 0; }
        free(enc_buf); free(dec_coeffs);
        /* Strict 2D huffman roundtrip across sizes: encode + decode must return
           the exact same coefficients (subband + 2x2 scan reversibility). */
        {
            int hf2 = 0;
            int szs[][2] = {{1,1},{2,2},{3,5},{4,4},{5,3},{7,9},{8,8},{16,16},{17,9},{1,64},{64,1},{2,7},{7,2}};
            int nsz = (int)(sizeof(szs)/sizeof(szs[0]));
            for (int mode = 0; mode < 2 && !hf2; mode++) {  /* 0=single, 1=ctx */
                for (int t = 0; t < nsz && !hf2; t++) {
                    int tw = szs[t][0], th = szs[t][1], tt = tw*th, spw = tw+2;
                    int16_t *src = calloc((size_t)spw*(th+2), sizeof(int16_t));
                    for (int y = 0; y < th; y++) {
                        for (int x = 0; x < tw; x++) {
                            int i = y*tw + x, r = (i*37+13)%97;
                            src[(size_t)(y+1)*spw + (x+1)] = (r<70)?0 : (int16_t)((r*7)%127-63);
                        }
                    }
                    int fq[NUM_HUFF_SYMBOLS]; count_frequencies(src, tw, th, fq);
                    int cl[NUM_HUFF_SYMBOLS]; uint32_t hc[NUM_HUFF_SYMBOLS];
                    build_huffman_codes(fq, NUM_HUFF_SYMBOLS, cl);
                    generate_canonical_codes(cl, NUM_HUFF_SYMBOLS, hc);
                    int cl1[NUM_HUFF_SYMBOLS]; uint32_t hc1[NUM_HUFF_SYMBOLS];
                    memcpy(cl1, cl, sizeof(cl)); memcpy(hc1, hc, sizeof(hc));
                    uint8_t *eb = malloc((size_t)tt*8+64);
                    Bitstream w; bitstream_init(&w, eb, (size_t)tt*8+64);
                    if (mode) huffman_encode_ctx(&w, src, tw, th, hc, cl, hc1, cl1);
                    else      huffman_encode_runval(&w, src, tw, th, hc, cl);
                    bitstream_flush(&w);
                    int el = bitstream_bytes(&w);
                    int16_t *dst = calloc((size_t)spw*(th+2), sizeof(int16_t));
                    Bitstream rb; bitstream_init(&rb, eb, el);
                    if (mode) huffman_decode_ctx(&rb, dst, tw, th, cl, hc, cl1, hc1);
                    else      huffman_decode_channel(&rb, dst, tw, th, cl, hc);
                    for (int y = 0; y < th && !hf2; y++)
                        for (int x = 0; x < tw && !hf2; x++)
                            if (src[(size_t)(y+1)*spw + (x+1)] != dst[(size_t)(y+1)*spw + (x+1)]) { hf2++; printf("  HF2D mode=%d %dx%d idx=%d: %d!=%d\n", mode, tw, th, y*tw+x, (int)src[(size_t)(y+1)*spw + (x+1)], (int)dst[(size_t)(y+1)*spw + (x+1)]); break; }
                    free(src); free(dst); free(eb);
                }
            }
            if (hf2 == 0) printf("[SELF-TEST HUFFMAN-2D] OK: %d sizes x 2 modes exact roundtrip\n", nsz);
            else { printf("[SELF-TEST HUFFMAN-2D] FAIL: %d\n", hf2); all_ok = 0; }
        }
        /* Self-test: EBCOT roundtrip (small block). 4x4 padded to 6x6. */
        int w = 4, h = 4, pw = w + 2;
        int16_t c[(4+2)*(4+2)] = { 0,0,0,0,0,0, 0,0,5,-3,0,0, 0,0,0,12,0,0, 0,0,-7,0,0,0, 0,1,0,0,0,0, 0,0,0,0,0,0};
        int bp = 0, mv = 0;
        for (int y = 1; y <= h; y++)
            for (int x = 1; x <= w; x++) { int av = abs(c[(size_t)y*pw + x]); if (av > mv) mv = av; }
        while(mv > 0) { bp++; mv >>= 1; }
        if(bp == 0) bp = 1;
        uint8_t *buf = (uint8_t*)malloc(4096);
        BacEnc e;
        bac_init_enc(&e, buf, 4096);
        ebcot_encode_channel(&e, c, w, h, NULL, 20, pw);
        int sz = bac_flush_enc(&e);
        int16_t *d=(int16_t*)calloc((size_t)pw*(h+2), sizeof(int16_t));
        BacDec dec;
        bac_init_dec(&dec, buf, sz);
        ebcot_decode_channel(&dec, d, w, h, NULL, 20, pw);
        errors = 0;
        for(int y = 0; y < h; y++)
            for(int x = 0; x < w; x++)
                if(c[(size_t)(y+1)*pw + (x+1)] != d[(size_t)(y+1)*pw + (x+1)])
                    { if(errors < 10) printf("[%d] %d!=%d\n", y*w+x, (int)c[(size_t)(y+1)*pw + (x+1)], (int)d[(size_t)(y+1)*pw + (x+1)]); errors++; }
        if(errors == 0)
            printf("[SELF-TEST EBCOT]  OK: 4x4 roundtrip, %d bytes\n", sz);
        else
            { printf("[SELF-TEST EBCOT]  FAIL: %d mismatches\n", errors); all_ok = 0; }
        free(buf); free(d);
        /* Stress-test: EBCOT roundtrip on pseudo-random coeffs w/ large range */
        {
            int sw = 48, sh = 48, st = sw*sh, spw = sw + 2;
            int16_t *sc = (int16_t*)calloc((size_t)spw*(sh+2), sizeof(int16_t));
            int16_t *sd = (int16_t*)calloc((size_t)spw*(sh+2), sizeof(int16_t));
            unsigned rng = 12345, fails = 0;
            for(int trial = 0; trial < 200 && fails == 0; trial++) {
                int maxbits = 1 + (trial % 15);  /* 1..15-bit magnitudes; include tiny */
                for(int y = 0; y < sh; y++) {
                    for(int x = 0; x < sw; x++) {
                        rng = rng*1664525u + 1013904223u;
                        int mag = (rng >> 9) & ((1 << maxbits) - 1);
                        /* make it sparse like real wavelet coeffs */
                        if(((rng >> 3) & 7) < 5) mag = 0;
                        sc[(size_t)(y+1)*spw + (x+1)] = (rng & 0x100) ? (int16_t)(-mag) : (int16_t)mag;
                    }
                }
                int sbp=0, smv=0;
                for(int y = 1; y <= sh; y++)
                    for(int x = 1; x <= sw; x++)
                        { int av = abs(sc[(size_t)y*spw + x]); if(av>smv) smv=av; }
                while(smv > 0) { sbp++; smv >>= 1; } if(sbp == 0) sbp=1;
                uint8_t *sbuf = (uint8_t*)malloc(st*6 + 4096);
                BacEnc se; bac_init_enc(&se, sbuf, st*6 + 4096);
                ebcot_encode_channel(&se, sc, sw, sh, NULL, 20, spw);
                int ssz=bac_flush_enc(&se);
                memset(sd, 0, (size_t)spw*(sh+2)*sizeof(int16_t));
                BacDec sdec;
                bac_init_dec(&sdec, sbuf, ssz);
                ebcot_decode_channel(&sdec, sd, sw, sh, NULL, 20, spw);
                for(int y = 0; y < sh; y++)
                    for(int x = 0; x < sw; x++)
                        if(sc[(size_t)(y+1)*spw + (x+1)] != sd[(size_t)(y+1)*spw + (x+1)]) { fails++; if(fails <= 3) printf("[STRESS trial %d bits %d] idx %d: %d!=%d\n", trial, maxbits, y*sw+x, (int)sc[(size_t)(y+1)*spw + (x+1)], (int)sd[(size_t)(y+1)*spw + (x+1)]); }
                free(sbuf);
            }
            if(fails == 0)
                printf("[SELF-TEST EBCOT-STRESS] OK: 200 trials 48x48 (BAC_CODE_BITS=%d)\n", BAC_CODE_BITS);
            else
                { printf("[SELF-TEST EBCOT-STRESS] FAIL: %u mismatches (BAC_CODE_BITS=%d)\n", fails, BAC_CODE_BITS); all_ok = 0; }
            free(sc); free(sd);
        }
        /* Stress-test: BAC roundtrip on many sizes (catches byte-renorm desync) */
        {
            int test_w[] = {2,3,4,5,7,8,11,13,16,17,23,31,32,37,47,64,0};
            unsigned rng = 999983, total_fails = 0, total_trials = 0;
            for (int si = 0; test_w[si] != 0; si++) {
                int sw = test_w[si], sh = sw + (si & 1);
                int st = sw * sh, spw = sw + 2;
                int16_t *sc = (int16_t*)calloc((size_t)spw*(sh+2), sizeof(int16_t));
                int16_t *sd = (int16_t*)calloc((size_t)spw*(sh+2), sizeof(int16_t));
                for (int trial = 0; trial < 20; trial++) {
                    int maxbits = 1 + (trial % 14);
                    for (int y = 0; y < sh; y++) {
                        for (int x = 0; x < sw; x++) {
                            rng = rng*1664525u + 1013904223u;
                            int mag = (rng >> 9) & ((1 << maxbits) - 1);
                            if (((rng >> 3) & 7) < 5) mag = 0;
                            sc[(size_t)(y+1)*spw + (x+1)] = (rng & 0x100) ? (int16_t)(-mag) : (int16_t)mag;
                        }
                    }
                    int sbp=0, smv=0;
                    for (int y=1;y<=sh;y++)for(int x=1;x<=sw;x++){int av=abs(sc[(size_t)y*spw+x]);if(av>smv)smv=av;}
                    while(smv>0){sbp++;smv>>=1;} if(sbp==0)sbp=1;
                    uint8_t *sbuf=(uint8_t*)malloc(st*6+4096);
                    BacEnc se; bac_init_enc(&se,sbuf,st*6+4096);
                    ebcot_encode_channel(&se, sc, sw, sh, NULL, 20, spw);
                    int ssz=bac_flush_enc(&se);
                    memset(sd,0,(size_t)spw*(sh+2)*sizeof(int16_t));
                    BacDec sdec; bac_init_dec(&sdec,sbuf,ssz);
                    ebcot_decode_channel(&sdec, sd, sw, sh, NULL, 20, spw);
                    unsigned f=0;
                    for(int y=0;y<sh;y++)for(int x=0;x<sw;x++) if(sc[(size_t)(y+1)*spw+(x+1)]!=sd[(size_t)(y+1)*spw+(x+1)]) f++;
                    if(f){total_fails++; printf("[BAC-SIZE %dx%d trial %d] %u mismatches\n",sw,sh,trial,f);}
                    total_trials++;
                    free(sbuf);
                }
                free(sc); free(sd);
            }
            if (total_fails == 0)
                printf("[SELF-TEST BAC-SIZES] OK: %d trials, %d sizes (%d..%d)\n",
                    total_trials, (int)(sizeof(test_w)/sizeof(test_w[0])-1), test_w[0], test_w[(sizeof(test_w)/sizeof(test_w[0]))-2]);
            else
                { printf("[SELF-TEST BAC-SIZES] FAIL: %u/%u mismatches\n", total_fails, total_trials); all_ok = 0; }
        }
        /* Self-test: wavelet roundtrip - comprehensive edge-case coverage */
        {
            /* All sizes: odd, even, non-power-of-2, tiny, SIMD boundaries */
            int test_sizes[][2] = {
                {1,1},{1,2},{2,1},{2,2},{1,3},{3,1},{1,7},{7,1},
                {3,3},{3,5},{5,3},{3,7},{7,3},
                {4,4},{5,5},{6,6},{7,7},{8,8},{9,9},
                {7,11},{11,7},{7,13},{13,7},{7,15},{15,7},
                {8,7},{7,8},{9,8},{8,9},
                {10,10},{11,11},{12,12},{13,13},{14,14},{15,15},
                {16,16},{17,17},{18,18},{19,19},{20,20},
                {21,21},{22,22},{23,23},{24,24},{25,25},
                {26,26},{27,27},{28,28},{29,29},{30,30},{31,31},
                {32,32},{33,33},{47,47},
                {15,31},{31,15},{17,33},{33,17},{21,47},{47,21},
                {31,47},{47,31},{33,65},{65,33},
                {64,64},{63,65},{65,63},
                {128,128},{127,129},{129,127},
                {256,256},{255,257},{257,255},
                {2,127},{127,2},{1,128},{128,1},
                {511,511},
                {0,0}
            };
            int n_sizes = 0, wfail = 0;
            for (int ti = 0; test_sizes[ti][0] != 0; ti++) n_sizes++;
            for (int ti = 0; test_sizes[ti][0] != 0; ti++) {
                int tw = test_sizes[ti][0], th = test_sizes[ti][1];
                int tt = tw * th;
                float *tf = (float*)malloc(tt*sizeof(float));
                float *ts = (float*)malloc(tt*sizeof(float));
                unsigned rng = (unsigned)(tw * 65537 + th * 257 + ti * 17);
                for (int i = 0; i < tt; i++) {
                    rng = rng*1664525u + 1013904223u;
                    tf[i] = (float)((int)(rng & 0xFF) - 128);
                    ts[i] = tf[i];
                }
                cdf97_forward_2d(tf, tw, th);
                cdf97_inverse_2d(tf, tw, th);
                float maxerr = 0; int badidx = -1;
                for (int i = 0; i < tt; i++) {
                    float e = fabsf(tf[i] - ts[i]);
                    if (e > maxerr) { maxerr = e; badidx = i; }
                }
                if (maxerr >= 1.0f) {
                    printf("[SELF-TEST WAVELET]  FAIL: %dx%d maxerr=%.4f (idx=%d r=%d c=%d)\n",
                        tw, th, maxerr, badidx, badidx/tw, badidx%tw);
                    all_ok = 0; wfail++;
                }
                free(tf); free(ts);
            }
            if (wfail == 0)
                printf("[SELF-TEST WAVELET]  OK: %d size combos (1x1 .. 511x511)\n", n_sizes);
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
            }
            rgb_to_yuv_batch(orig, 0, 3, tw, th, ty, tu, tv, NULL);
            cdf97_forward_2d(ty, tw, th);
            cdf97_forward_2d(tu, tw, th);
            cdf97_forward_2d(tv, tw, th);
            int16_t *qy = (int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t)), *qu = (int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t)), *qv = (int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t));
            quantize_coeffs(ty, qy, tw, th, compute_base(1), g_qt_y); quantize_coeffs(tu, qu, tw, th, compute_base(1), g_qt_c); quantize_coeffs(tv, qv, tw, th, compute_base(1), g_qt_c);
            dequantize_channel(qy, ty, tw, th, compute_base(1), g_qt_y); dequantize_channel(qu, tu, tw, th, compute_base(1), g_qt_c); dequantize_channel(qv, tv, tw, th, compute_base(1), g_qt_c);
            cdf97_inverse_2d(ty, tw, th);
            cdf97_inverse_2d(tu, tw, th);
            cdf97_inverse_2d(tv, tw, th);
            yuv_to_rgb_batch(ty, tu, tv, NULL, tw, th, dec, 0, 3);
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
            }
            rgb_to_yuv_batch(orig, 0, 3, tw, th, ty, tu, tv, NULL);
            cdf97_forward_2d(ty, tw, th);
            cdf97_forward_2d(tu, tw, th);
            cdf97_forward_2d(tv, tw, th);
            qy = (int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t)); qu=(int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t)); qv=(int16_t*)malloc((size_t)(tw+2)*(th+2) * sizeof(int16_t));
            quantize_coeffs(ty, qy, tw, th, compute_base(1), g_qt_y);
            quantize_coeffs(tu, qu, tw, th, compute_base(1), g_qt_c);
            quantize_coeffs(tv, qv, tw, th, compute_base(1), g_qt_c);
            dequantize_channel(qy, ty, tw, th, compute_base(1), g_qt_y);
            dequantize_channel(qu, tu, tw, th, compute_base(1), g_qt_c);
            dequantize_channel(qv, tv, tw, th, compute_base(1), g_qt_c);
            cdf97_inverse_2d(ty, tw, th);
            cdf97_inverse_2d(tu, tw, th);
            cdf97_inverse_2d(tv, tw, th);
            yuv_to_rgb_batch(ty, tu, tv, NULL, tw, th, dec, 0, 3);
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
        /* Full encode/decode roundtrip on narrow (1xN / Nx1) images.
           The wavelet-only test covers 1xN, but the full pipeline (chroma
           down/up, quant, entropy) has its own width/height asymmetries; a
           1xN image must not decode to garbage while Nx1 is fine. */
        {
            int nfail = 0;
            int narrow[][2] = {{1,16},{16,1},{1,64},{64,1},{1,256},{256,1},{2,64},{64,2},{1,3},{3,1},{1,2},{2,1}};
            int nn = (int)(sizeof(narrow)/sizeof(narrow[0]));
            for (int t = 0; t < nn; t++) {
                int tw = narrow[t][0], th = narrow[t][1], tt = tw*th;
                uint8_t *orig = (uint8_t*)malloc(tt*3);
                unsigned rng = 777 + (unsigned)(tw*31 + th*7);
                for (int i = 0; i < tt*3; i++) { rng = rng*1664525u + 1013904223u; orig[i] = (uint8_t)(rng >> 24); }
                /* Test BOTH entropy coders: a mode-specific bug (e.g. EBCOT at
                   width==1) must not hide behind the auto pick. */
                for (int mode = 1; mode <= 2; mode++) {  /* 1=huffman, 2=ebcot */
                    wtpc_enc_info info;
                    unsigned char *enc = wtpc_encode_mem(orig, &info, tw, th, 0, 30, 0, mode, 0, 0, 0, 0);
                    if (!enc) { nfail++; printf("  NARROW %dx%d mode=%d: encode failed\n", tw, th, mode); continue; }
                    int dw = 0, dh = 0, dq = 0, dcomp = 0;
                    unsigned char *dec = wtpc_decode_mem(enc, info.encoded_bytes, &dw, &dh, &dq, &dcomp);
                    free(enc);
                    if (!dec || dw != tw || dh != th) { nfail++; printf("  NARROW %dx%d mode=%d: decode failed\n", tw, th, mode); free(dec); continue; }
                    double mse = 0;
                    for (int i = 0; i < tt*3; i++) { double d = (double)orig[i] - (double)dec[i]; mse += d*d; }
                    mse /= (tt*3);
                    double psnr = (mse > 0) ? 10.0*log10(255.0*255.0/mse) : 99.0;
                    if (psnr < 20.0) { nfail++; printf("  NARROW %dx%d mode=%d: psnr=%.2f dB (broken)\n", tw, th, mode, psnr); }
                    free(dec);
                }
                free(orig);
            }
            if (nfail == 0) printf("[SELF-TEST NARROW] OK: %d narrow sizes x 2 modes roundtrip psnr>=20 dB\n", nn);
            else { printf("[SELF-TEST NARROW] FAIL: %d broken\n", nfail); all_ok = 0; }
        }
        /* Bitstream edge case tests */
        {
            uint8_t buf[256]; int fails = 0;
            /* Test 1: simple put/get roundtrip, various bit sizes */
            { Bitstream w; bitstream_init(&w, buf, sizeof(buf));
              for (int nb = 1; nb <= 16; nb++) put_bits(&w, (1u << nb) - 1, nb);
              bitstream_flush(&w);
              Bitstream r; bitstream_init(&r, buf, bitstream_bytes(&w));
              for (int nb = 1; nb <= 16; nb++) {
                  uint32_t v = get_bits(&r, nb);
                  if (v != (1u << nb) - 1) { fails++; printf("  BS-EDGE put/get nb=%d fail: got %u\n", nb, v); }
              } }
            /* Test 2: mixed sizes crossing refill boundaries */
            { Bitstream w; bitstream_init(&w, buf, sizeof(buf));
              for (int n = 0; n < 8; n++) { put_bits(&w, 0xAA, 8); }
              bitstream_flush(&w);
              Bitstream r; bitstream_init(&r, buf, bitstream_bytes(&w));
              /* Read 1 bit at a time through several refills */
              for (int n = 0; n < 64; n++) {
                  uint32_t v = get_bits(&r, 1);
                  int expected = (0xAA >> (7 - (n & 7))) & 1;
                  if (v != (uint32_t)expected) { fails++; printf("  BS-EDGE 1-bit n=%d fail: got %u expected %d\n", n, v, expected); break; }
              } }
            /* Test 3: read exactly at cache boundary (bits_in_cache == num_bits) */
            { Bitstream w; bitstream_init(&w, buf, sizeof(buf));
              put_bits(&w, 0x12, 8); put_bits(&w, 0x34, 8); put_bits(&w, 0x56, 8); put_bits(&w, 0x78, 8);
              bitstream_flush(&w);
              Bitstream r; bitstream_init(&r, buf, bitstream_bytes(&w));
              /* Read 8 bits exactly at initial fill (bits_in_cache goes from 32 to 25 after init): read 25 */
              uint32_t v = get_bits(&r, 8);
              if (v != 0x12) { fails++; printf("  BS-EDGE boundary fail: got 0x%02x\n", v); }
              /* Now at boundary: read 17 bits to force refill */
              v = get_bits(&r, 17);
              if (v != 0x068AC) { fails++; printf("  BS-EDGE refill fail: got 0x%05x\n", v); }
              v = get_bits(&r, 7);
              if (v != (0x78 & 0x7F)) { fails++; printf("  BS-EDGE tail fail: got 0x%02x\n", v); } }
            /* Test 4: get_eg roundtrip */
            { uint32_t v_in[] = {0,1,2,3,7,15,31,100,1000,0xFFF,0xFFFF};
              Bitstream w; bitstream_init(&w, buf, sizeof(buf));
              for (int i = 0; i < (int)(sizeof(v_in)/sizeof(v_in[0])); i++) put_eg(&w, v_in[i]);
              bitstream_flush(&w);
              Bitstream r; bitstream_init(&r, buf, bitstream_bytes(&w));
              for (int i = 0; i < (int)(sizeof(v_in)/sizeof(v_in[0])); i++) {
                  uint32_t v = get_eg(&r);
                  if (v != v_in[i]) { fails++; printf("  BS-EDGE get_eg fail: got %u expected %u\n", v, v_in[i]); }
              } }
            /* Test 5: zero bit read */
            { Bitstream r; bitstream_init(&r, buf, 0);
              uint32_t v = get_bits(&r, 0);
              if (v != 0) { fails++; printf("  BS-EDGE get_bits(0) fail: got %u\n", v); } }
            /* Test 6: empty buffer, no put, get should return 0 */
            { uint8_t zbuf[4] = {0,0,0,0};
              Bitstream r; bitstream_init(&r, zbuf, 4);
              uint32_t v = get_bits(&r, 32);
              if (v != 0) { fails++; printf("  BS-EDGE empty buffer fail: got 0x%08x\n", v); } }
            /* Test 7: chained refill (25 bits crossing 32-bit cache boundary) */
            { Bitstream w; bitstream_init(&w, buf, sizeof(buf));
              put_bits(&w, 0x12345678, 32); put_bits(&w, 1, 1);
              bitstream_flush(&w);
              Bitstream r; bitstream_init(&r, buf, bitstream_bytes(&w));
              uint32_t v = get_bits(&r, 25);
              uint32_t expected = (uint32_t)((0x12345678ull >> 7) & 0x1FFFFFF);
              if (v != expected) { fails++; printf("  BS-EDGE 25-bit fail: got 0x%lx expected 0x%lx\n", (unsigned long)v, (unsigned long)expected); }
              v = get_bits(&r, 8);
              if (v != (uint32_t)(((0x12345678 & 0x7F) << 1) | 1)) { fails++; printf("  BS-EDGE remainder fail: got 0x%02x\n", v); } }
            if (fails == 0)
                printf("[SELF-TEST BITSTREAM] OK: edge cases passed\n");
            else
                { printf("[SELF-TEST BITSTREAM] FAIL: %d failures\n", fails); all_ok = 0; }
        }
        /* Self-test: wavelet hash roundtrip (thumbhash-like ultra-compact) */
        {
            uint8_t rgb[4*4*3];
            for (int i = 0; i < 4*4; i++) { rgb[i*3]=64; rgb[i*3+1]=128; rgb[i*3+2]=192; }
            int hlen;
            uint8_t *hash = wtpc_hash_encode_mem(rgb, 4, 4, &hlen);
            if (!hash || hlen < 3) {
                printf("[SELF-TEST HASH] FAIL: encode=%d\n", hlen); all_ok = 0; free(hash);
            } else {
                int dw, dh;
                uint8_t *dec = wtpc_hash_decode_mem(hash, hlen, &dw, &dh);
                free(hash);
                if (!dec || dw != 4 || dh != 4) {
                    printf("[SELF-TEST HASH] FAIL: decode\n"); all_ok = 0; free(dec);
                } else {
                    double err = 0; for (int i = 0; i < 4*4*3; i++) { double d = (double)rgb[i] - dec[i]; err += d*d; }
                    err = sqrt(err / (4*4*3));
                    if (err < 50.0)
                        printf("[SELF-TEST HASH] OK: 4x4 roundtrip, %d bytes, rmse=%.1f\n", hlen, err);
                    else
                        { printf("[SELF-TEST HASH] FAIL: rmse=%.1f\n", err); all_ok = 0; }
                    free(dec);
                }
            }
        }
        if(!all_ok) return 1;
    }
#ifdef _WIN32    
    timeEndPeriod(1);
#endif
    return 0;
}
