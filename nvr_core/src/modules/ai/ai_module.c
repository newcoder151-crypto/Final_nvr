/**
 * @file ai_module.c
 * @brief AI analytics implementation
 *
 * Pipeline per detection call:
 *   a. Convert I420 tile -> 640x640 RGB float32 [0,1] (letterboxed).
 *   b. Run YOLO OrtSession -> bounding boxes.
 *   c. Decode YOLO output [1, 5, num_anchors] -> FaceBox list.
 *   d. Apply confidence threshold + greedy NMS.
 *   e. For each kept box: crop face region -> run MobileFaceNet -> 128-dim embedding.
 *   f. Cosine similarity vs. per-camera EmbedDedupTable entries.
 *        >= EMBED_COSINE_THRESHOLD -> duplicate (is_unique=0)
 *        <  threshold for all      -> new group  (is_unique=1, store embedding)
 *   g. Save colour JPEG crop (unique faces only).
 *   h. db_insert_face_with_dedup() stores face + embedding + group info.
 *   i. Fire AI_EVENT_FACE_DETECTED.
 *
 * MobileFaceNet model notes:
 *   Input  name: "input"  shape [1, 3, 112, 112]  dtype float32
 *                values normalised to [-1, 1]  (pixel/128 - 1)
 *   Output name: "output" shape [1, 128]           dtype float32
 *                model should output L2-normalised vectors; we re-normalise
 *                anyway to be safe.
 *   If your exported model uses different input/output names, adjust
 *   EMBED_INPUT_NAME / EMBED_OUTPUT_NAME below.
 *
 * Build:
 *   With ONNX RT : -DMNVR_WITH_ONNX -lonnxruntime
 *   Without      : face detection + embedding compile to no-ops, zero extra deps.
 */

#define _POSIX_C_SOURCE 200809L

#include "ai_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include "../../db/db_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <jpeglib.h>

/* ONNX input/output names — adjust if your model uses different names */
#define YOLO_INPUT_NAME    "images"
#define YOLO_OUTPUT_NAME   "output0"
#define EMBED_INPUT_NAME   "input.1"
#define EMBED_OUTPUT_NAME  "516"

/* =========================================================================
 * Motion detection — central ROI
 * ========================================================================= */

#define PIXEL_DIFF_THRESHOLD  20

static float compute_motion_roi(const uint8_t *cur, const uint8_t *prev,
                                 int width, int height, float roi_fraction)
{
    int margin_x = (int)(width  * (1.0f - roi_fraction) * 0.5f);
    int margin_y = (int)(height * (1.0f - roi_fraction) * 0.5f);
    int x0 = margin_x, x1 = width  - margin_x;
    int y0 = margin_y, y1 = height - margin_y;

    int changed = 0;
    int total   = (x1 - x0) * (y1 - y0);
    if (total <= 0) return 0.0f;

    for (int row = y0; row < y1; row++) {
        const uint8_t *cr = cur  + row * width + x0;
        const uint8_t *pr = prev + row * width + x0;
        for (int col = x0; col < x1; col++, cr++, pr++) {
            int diff = (int)*cr - (int)*pr;
            if (diff < 0) diff = -diff;
            if (diff > PIXEL_DIFF_THRESHOLD) changed++;
        }
    }
    return (float)changed / (float)total;
}

/* =========================================================================
 * RDAS stub
 * ========================================================================= */

static float compute_rdas(CamAiContext *cam,
                           const uint8_t *y, int w, int h)
{
    (void)cam; (void)y; (void)w; (void)h;
    return 0.0f;
}

/* =========================================================================
 * Colour face crop — save I420 region as RGB JPEG using libjpeg
 * ========================================================================= */

static int save_face_crop_colour(
        const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
        int fw, int fh,
        float y_min, float x_min, float y_max, float x_max,
        const char *path)
{
    float pad_y = (y_max - y_min) * FACE_CROP_PADDING;
    float pad_x = (x_max - x_min) * FACE_CROP_PADDING;

    int cx1 = (int)((x_min - pad_x) * fw);
    int cy1 = (int)((y_min - pad_y) * fh);
    int cx2 = (int)((x_max + pad_x) * fw);
    int cy2 = (int)((y_max + pad_y) * fh);
    if (cx1 < 0) cx1 = 0;  if (cy1 < 0) cy1 = 0;
    if (cx2 > fw) cx2 = fw; if (cy2 > fh) cy2 = fh;

    int cw = cx2 - cx1;
    int ch = cy2 - cy1;
    if (cw < FACE_CROP_MIN_PX || ch < FACE_CROP_MIN_PX) return 0;

    char dir[MNVR_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }

    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;

    uint8_t *row_rgb = malloc((size_t)cw * 3);
    if (!row_rgb) { fclose(fp); return 0; }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = (JDIMENSION)cw;
    cinfo.image_height     = (JDIMENSION)ch;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, FACE_CROP_JPEG_Q, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    int uv_stride = fw / 2;

    while ((int)cinfo.next_scanline < ch) {
        int row = cy1 + (int)cinfo.next_scanline;
        for (int col = 0; col < cw; col++) {
            int sx = cx1 + col;
            int Y  = y_plane[row * fw + sx];
            int U  = u_plane[(row / 2) * uv_stride + sx / 2] - 128;
            int V  = v_plane[(row / 2) * uv_stride + sx / 2] - 128;

            int R = Y + (int)(1.402f   * V);
            int G = Y - (int)(0.344f   * U) - (int)(0.714f * V);
            int B = Y + (int)(1.772f   * U);
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;

            row_rgb[col * 3 + 0] = (uint8_t)R;
            row_rgb[col * 3 + 1] = (uint8_t)G;
            row_rgb[col * 3 + 2] = (uint8_t)B;
        }
        JSAMPROW rows[1] = { row_rgb };
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row_rgb);
    fclose(fp);
    return 1;
}

/* =========================================================================
 * Embedding math helpers
 * ========================================================================= */

/* L2-normalise a EMBED_DIM-float vector in-place.
 * After this, ||vec|| == 1 and cosine_similarity(a,b) == dot(a,b). */
static void l2_normalize(float *vec, int dim)
{
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) sum += vec[i] * vec[i];
    if (sum < 1e-10f) return;   /* zero vector, leave as-is */
    float inv = 1.0f / sqrtf(sum);
    for (int i = 0; i < dim; i++) vec[i] *= inv;
}

/* Cosine similarity of two L2-normalised EMBED_DIM vectors.
 * Returns value in [-1, 1].  Same person typically > 0.5. */
static float cosine_similarity(const float *a, const float *b, int dim)
{
    float dot = 0.0f;
    for (int i = 0; i < dim; i++) dot += a[i] * b[i];
    return dot;   /* both already unit-norm */
}

/* =========================================================================
 * Temporal bbox cooldown
 * ========================================================================= */

/**
 * temporal_cooldown_check()
 *
 * Returns true if this bbox centre was seen recently (within
 * TEMPORAL_COOLDOWN_MS) and is spatially close to a recorded slot
 * (within TEMPORAL_COOLDOWN_DIST in normalised coords).
 *
 * If NOT in cooldown: records the slot and returns false (caller proceeds).
 * If     in cooldown: returns true (caller skips expensive inference).
 */
static bool temporal_cooldown_check(TemporalCooldownTable *tbl,
                                     float cx, float cy,
                                     uint64_t pts_ms)
{
    for (int i = 0; i < TEMPORAL_COOLDOWN_SLOTS; i++) {
        if (!tbl->slots[i].valid) continue;
        if (pts_ms - tbl->slots[i].last_ms > TEMPORAL_COOLDOWN_MS) {
            tbl->slots[i].valid = false;  /* expired */
            continue;
        }
        float dx = cx - tbl->slots[i].cx;
        float dy = cy - tbl->slots[i].cy;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist < TEMPORAL_COOLDOWN_DIST) {
            /* Same face, still warm — update timestamp, suppress */
            tbl->slots[i].last_ms = pts_ms;
            return true;
        }
    }
    /* Not found — register new slot */
    int s = tbl->next % TEMPORAL_COOLDOWN_SLOTS;
    tbl->slots[s].cx      = cx;
    tbl->slots[s].cy      = cy;
    tbl->slots[s].last_ms = pts_ms;
    tbl->slots[s].valid   = true;
    tbl->next = (tbl->next + 1) % TEMPORAL_COOLDOWN_SLOTS;
    return false;
}

/* =========================================================================
 * Face alignment — affine warp to 112×112 canonical pose
 *
 * Reference landmarks (ArcFace/MobileFaceNet standard, 112×112 space):
 *   left-eye    (38.29, 51.69)
 *   right-eye   (73.53, 51.50)
 *   nose        (56.02, 71.74)
 *   mouth-left  (41.55, 92.37)
 *   mouth-right (70.73, 92.20)
 *
 * We compute a similarity transform (scale + rotation + translation) from
 * the detected 5-point landmarks to these references using the least-squares
 * closed-form solution, then apply it with bilinear sampling.
 *
 * If landmarks are unavailable we fall back to the existing nearest-neighbour
 * crop used by yuv_crop_to_embed_input() — no regression.
 * ========================================================================= */

/* Reference landmarks in 112×112 space */
static const float kRefLmX[5] = { 38.29f, 73.53f, 56.02f, 41.55f, 70.73f };
static const float kRefLmY[5] = { 51.69f, 51.50f, 71.74f, 92.37f, 92.20f };

/**
 * solve_similarity_transform_2x3()
 *
 * Fits a 2×3 affine matrix M = [a -b tx; b a ty] (similarity, no shear)
 * mapping src landmarks -> dst landmarks using the Umeyama closed-form.
 *
 * out_m[0..5] = { a, -b, tx, b, a, ty }
 */
static void solve_similarity_transform_2x3(
        const float *sx, const float *sy,   /* 5 source points */
        const float *dx, const float *dy,   /* 5 dest   points */
        float *out_m)
{
    const int N = 5;
    float mx_s = 0, my_s = 0, mx_d = 0, my_d = 0;
    for (int i = 0; i < N; i++) {
        mx_s += sx[i]; my_s += sy[i];
        mx_d += dx[i]; my_d += dy[i];
    }
    mx_s /= N; my_s /= N; mx_d /= N; my_d /= N;

    float a = 0, b = 0, sigma2 = 0;
    for (int i = 0; i < N; i++) {
        float csx = sx[i] - mx_s, csy = sy[i] - my_s;
        float cdx = dx[i] - mx_d, cdy = dy[i] - my_d;
        a      += csx * cdx + csy * cdy;
        b      += csx * cdy - csy * cdx;
        sigma2 += csx * csx + csy * csy;
    }
    if (sigma2 < 1e-6f) {
        /* Degenerate — identity-ish fallback */
        out_m[0]=1; out_m[1]=0; out_m[2]=0;
        out_m[3]=0; out_m[4]=1; out_m[5]=0;
        return;
    }
    float scale = 1.0f / sigma2;
    a *= scale;
    b *= scale;

    float tx = mx_d - a * mx_s + b * my_s;
    float ty = my_d - b * mx_s - a * my_s;

    out_m[0] =  a;  out_m[1] = -b;  out_m[2] = tx;
    out_m[3] =  b;  out_m[4] =  a;  out_m[5] = ty;
}

/**
 * yuv_aligned_to_embed_input()
 *
 * Warp-crops the face using a precomputed 2×3 similarity matrix,
 * producing a 112×112 RGB float32 NCHW tensor normalised to [-1, 1].
 * Uses bilinear sampling for quality.
 */
static void yuv_aligned_to_embed_input(
        const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
        int fw, int fh,
        const float *m,         /* 2x3 similarity matrix (src pixel → dst 112px) */
        float *out)             /* EMBED_INPUT_W * EMBED_INPUT_H * 3, NCHW */
{
    const int dw = EMBED_INPUT_W;   /* 112 */
    const int dh = EMBED_INPUT_H;   /* 112 */
    int plane_size = dw * dh;
    int uv_stride  = fw / 2;

    /* Inverse transform: dst -> src, so we can sample src for each dst pixel */
    /* M = [a -b tx; b a ty].  det = a^2 + b^2 */
    float a = m[0], b_v = m[3];
    float det = a * a + b_v * b_v;
    if (det < 1e-8f) { memset(out, 0, (size_t)(plane_size * 3) * sizeof(float)); return; }
    float inv_det = 1.0f / det;
    float ia =  a  * inv_det;
    float ib = -b_v * inv_det;    /* inv[0,1] = -b/det for similarity inverse */
    float itx = -(ia * m[2] + ib * m[5]);   /* will be corrected below */
    /* Full inverse of [a -b tx; b a ty]:
     *   [ a   b  -(a*tx + b*ty) ] * 1/det
     *   [-b   a  -(-b*tx+ a*ty) ]
     */
    float inv_m[6];
    inv_m[0] =  a   * inv_det;
    inv_m[1] =  b_v * inv_det;
    inv_m[2] = -(a   * m[2] + b_v * m[5]) * inv_det;
    inv_m[3] = -b_v * inv_det;
    inv_m[4] =  a   * inv_det;
    inv_m[5] = -(-b_v * m[2] + a * m[5]) * inv_det;
    (void)ia; (void)ib; (void)itx; /* suppress unused-var warnings */

    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            /* Map dst pixel centre to src pixel coords */
            float fx = inv_m[0] * (dx + 0.5f) + inv_m[1] * (dy + 0.5f) + inv_m[2];
            float fy = inv_m[3] * (dx + 0.5f) + inv_m[4] * (dy + 0.5f) + inv_m[5];

            /* Bilinear sample */
            int   x0 = (int)fx, y0 = (int)fy;
            int   x1 = x0 + 1, y1 = y0 + 1;
            float wx = fx - (float)x0, wy = fy - (float)y0;

            /* Clamp to frame bounds */
            if (x0 < 0) x0 = 0; if (x0 >= fw) x0 = fw - 1;
            if (x1 < 0) x1 = 0; if (x1 >= fw) x1 = fw - 1;
            if (y0 < 0) y0 = 0; if (y0 >= fh) y0 = fh - 1;
            if (y1 < 0) y1 = 0; if (y1 >= fh) y1 = fh - 1;

            /* Sample 4 corners */
            int uv_x0 = x0 / 2, uv_x1 = x1 / 2;
            int uv_y0 = y0 / 2, uv_y1 = y1 / 2;

            auto inline int yuv2r(int Y,int U,int V){ (void)U; return Y+(int)(1.402f*V); }
            auto inline int yuv2g(int Y,int U,int V){ return Y-(int)(0.344f*U)-(int)(0.714f*V); }
            auto inline int yuv2b(int Y,int U,int V){ (void)V; return Y+(int)(1.772f*U); }

#define SAMPLE_RGB(py, px, r_, g_, b_) do { \
    int _Y = y_plane[(py)*fw+(px)]; \
    int _U = u_plane[uv_y##_dmy * uv_stride + uv_x##_dmx] - 128; \
    int _V = v_plane[uv_y##_dmy * uv_stride + uv_x##_dmx] - 128; \
    (r_) = _Y + (int)(1.402f * _V); \
    (g_) = _Y - (int)(0.344f * _U) - (int)(0.714f * _V); \
    (b_) = _Y + (int)(1.772f * _U); \
} while(0)

            /* Inline 4-tap bilinear per-channel */
            float R = 0, G = 0, B = 0;
            {
                int _Y, _U, _V, _r, _g, _b;
#define TAP(py, px, uvpy, uvpx, wt) \
    _Y = y_plane[(py)*fw+(px)]; \
    _U = u_plane[(uvpy)*uv_stride+(uvpx)] - 128; \
    _V = v_plane[(uvpy)*uv_stride+(uvpx)] - 128; \
    _r = _Y + (int)(1.402f*_V); \
    _g = _Y - (int)(0.344f*_U) - (int)(0.714f*_V); \
    _b = _Y + (int)(1.772f*_U); \
    R += (wt) * (float)(_r < 0 ? 0 : _r > 255 ? 255 : _r); \
    G += (wt) * (float)(_g < 0 ? 0 : _g > 255 ? 255 : _g); \
    B += (wt) * (float)(_b < 0 ? 0 : _b > 255 ? 255 : _b);

                TAP(y0, x0, uv_y0, uv_x0, (1-wx)*(1-wy))
                TAP(y0, x1, uv_y0, uv_x1, (  wx)*(1-wy))
                TAP(y1, x0, uv_y1, uv_x0, (1-wx)*(  wy))
                TAP(y1, x1, uv_y1, uv_x1, (  wx)*(  wy))
#undef TAP
            }
#undef SAMPLE_RGB

            int idx = dy * dw + dx;
            out[idx]                  = R / 128.0f - 1.0f;
            out[idx + plane_size]     = G / 128.0f - 1.0f;
            out[idx + plane_size * 2] = B / 128.0f - 1.0f;
        }
    }
}

/**
 * embed_dedup_check_and_insert()
 *
 * Searches the rolling table for an entry whose cosine similarity to `vec`
 * meets or exceeds EMBED_COSINE_THRESHOLD.
 *
 * Returns:
 *   match_idx >= 0  — index of the matching representative (duplicate)
 *   -1              — no match found (unique face); the new embedding is
 *                     inserted as a new group representative.
 *
 * `vec` must already be L2-normalised.
 */
static int embed_dedup_check_and_insert(EmbedDedupTable *tbl,
                                         const float *vec)
{
    float best_sim = -2.0f;
    int   best_idx = -1;

    int n = tbl->count < EMBED_DEDUP_WINDOW ? tbl->count : EMBED_DEDUP_WINDOW;
    for (int i = 0; i < n; i++) {
        if (!tbl->entries[i].valid) continue;
        float sim = cosine_similarity(vec, tbl->entries[i].vec, EMBED_DIM);
        printf("Similarity with entry %d = %.4f\n", i, sim);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }
    printf("BEST SIMILARITY = %.4f\n", best_sim);
    if (best_idx >= 0 && best_sim >= EMBED_COSINE_THRESHOLD){
        printf("DUPLICATE FACE DETECTED\n");
        return best_idx;   /* duplicate — return matching entry index */
    }

    printf("NEW UNIQUE FACE\n");
    /* Unique — insert as new representative */
    int slot = tbl->next;
    memcpy(tbl->entries[slot].vec, vec, EMBED_DIM * sizeof(float));
    tbl->entries[slot].valid = true;
    tbl->next = (tbl->next + 1) % EMBED_DEDUP_WINDOW;
    if (tbl->count < EMBED_DEDUP_WINDOW) tbl->count++;
    return -1;   /* unique */
}

/* =========================================================================
 * ONNX Runtime — shared helpers
 * ========================================================================= */

#ifdef MNVR_WITH_ONNX

static const OrtApi *g_ort = NULL;

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* =========================================================================
 * ONNX Runtime — YOLOv8-face inference
 * ========================================================================= */

/* Convert I420 -> 640x640 RGB float32 NCHW, letterboxed, normalised [0,1]. */
static void yuv_to_yolo_input(
        const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
        int fw, int fh,
        float *out,
        float *letterbox_scale,
        float *pad_x,
        float *pad_y)
{
    const int dw = YOLO_INPUT_W;
    const int dh = YOLO_INPUT_H;
    int uv_stride = fw / 2;

    float scale_x = (float)dw / (float)fw;
    float scale_y = (float)dh / (float)fh;
    float scale   = scale_x < scale_y ? scale_x : scale_y;
    int   new_w   = (int)(fw * scale + 0.5f);
    int   new_h   = (int)(fh * scale + 0.5f);
    float off_x   = ((float)dw - (float)new_w) * 0.5f;
    float off_y   = ((float)dh - (float)new_h) * 0.5f;

    *letterbox_scale = scale;
    *pad_x           = off_x;
    *pad_y           = off_y;

    float grey = 114.0f / 255.0f;
    int plane_size = dw * dh;
    for (int p = 0; p < plane_size * 3; p++) out[p] = grey;

    for (int dy = 0; dy < new_h; dy++) {
        float fy = ((float)dy + 0.5f) / scale - 0.5f;
        int   sy = (int)fy;
        if (sy < 0) sy = 0;
        if (sy >= fh) sy = fh - 1;

        for (int dx = 0; dx < new_w; dx++) {
            float fx = ((float)dx + 0.5f) / scale - 0.5f;
            int   sx = (int)fx;
            if (sx < 0) sx = 0;
            if (sx >= fw) sx = fw - 1;

            int Y = y_plane[sy * fw + sx];
            int U = u_plane[(sy / 2) * uv_stride + sx / 2] - 128;
            int V = v_plane[(sy / 2) * uv_stride + sx / 2] - 128;

            float R = clampf((float)(Y + (int)(1.402f * V)), 0, 255) / 255.0f;
            float G = clampf((float)(Y - (int)(0.344f * U) - (int)(0.714f * V)), 0, 255) / 255.0f;
            float B = clampf((float)(Y + (int)(1.772f * U)), 0, 255) / 255.0f;

            int out_y = (int)(off_y + dy);
            int out_x = (int)(off_x + dx);
            if (out_y < 0 || out_y >= dh || out_x < 0 || out_x >= dw) continue;

            int idx = out_y * dw + out_x;
            out[idx]                  = R;
            out[idx + plane_size]     = G;
            out[idx + plane_size * 2] = B;
        }
    }
}

/* =========================================================================
 * ONNX Runtime — MobileFaceNet embedding preprocessing
 *
 * Crops the face region from I420, resizes to 112x112, converts to RGB
 * float32 NCHW, normalises to [-1, 1]  (pixel/128 - 1).
 * Nearest-neighbour resize is fast and sufficient for embedding.
 * ========================================================================= */
static void yuv_crop_to_embed_input(
        const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
        int fw, int fh,
        float y_min, float x_min, float y_max, float x_max,
        float *out)     /* EMBED_INPUT_W * EMBED_INPUT_H * 3 floats, NCHW */
{
    /* Face crop region in pixel coords (with padding, same as JPEG crop) */
    float pad_y = (y_max - y_min) * FACE_CROP_PADDING;
    float pad_x = (x_max - x_min) * FACE_CROP_PADDING;

    int cx1 = (int)((x_min - pad_x) * fw);
    int cy1 = (int)((y_min - pad_y) * fh);
    int cx2 = (int)((x_max + pad_x) * fw);
    int cy2 = (int)((y_max + pad_y) * fh);
    if (cx1 < 0) cx1 = 0;  if (cy1 < 0) cy1 = 0;
    if (cx2 > fw) cx2 = fw; if (cy2 > fh) cy2 = fh;

    int cw = cx2 - cx1;
    int ch = cy2 - cy1;

    const int dw = EMBED_INPUT_W;
    const int dh = EMBED_INPUT_H;
    int plane_size = dw * dh;
    int uv_stride  = fw / 2;

    if (cw < 4 || ch < 4) {
        /* Degenerate crop — zero the buffer (embedding will be discarded). */
        memset(out, 0, plane_size * 3 * sizeof(float));
        return;
    }

    for (int dy = 0; dy < dh; dy++) {
        /* Nearest-neighbour sample row in source crop */
        int sy = cy1 + (int)((dy + 0.5f) * (float)ch / (float)dh);
        if (sy >= fh) sy = fh - 1;

        for (int dx = 0; dx < dw; dx++) {
            int sx = cx1 + (int)((dx + 0.5f) * (float)cw / (float)dw);
            if (sx >= fw) sx = fw - 1;

            int Y = y_plane[sy * fw + sx];
            int U = u_plane[(sy / 2) * uv_stride + sx / 2] - 128;
            int V = v_plane[(sy / 2) * uv_stride + sx / 2] - 128;

            /* BT.601 YCbCr -> RGB clamped to [0,255] */
            float R = clampf((float)(Y + (int)(1.402f * V)), 0.0f, 255.0f);
            float G = clampf((float)(Y - (int)(0.344f * U) - (int)(0.714f * V)), 0.0f, 255.0f);
            float B = clampf((float)(Y + (int)(1.772f * U)), 0.0f, 255.0f);

            /* Normalise to [-1, 1] as expected by MobileFaceNet */
            int idx = dy * dw + dx;
            out[idx]                  = R / 128.0f - 1.0f;
            out[idx + plane_size]     = G / 128.0f - 1.0f;
            out[idx + plane_size * 2] = B / 128.0f - 1.0f;
        }
    }
}

/* -------------------------------------------------------------------------
 * IoU + NMS
 * ------------------------------------------------------------------------- */
static float box_iou(const FaceBox *a, const FaceBox *b)
{
    float iy1 = a->y_min > b->y_min ? a->y_min : b->y_min;
    float ix1 = a->x_min > b->x_min ? a->x_min : b->x_min;
    float iy2 = a->y_max < b->y_max ? a->y_max : b->y_max;
    float ix2 = a->x_max < b->x_max ? a->x_max : b->x_max;
    float ih = iy2 - iy1, iw = ix2 - ix1;
    if (ih <= 0 || iw <= 0) return 0.0f;
    float inter = ih * iw;
    float ua = (a->y_max - a->y_min) * (a->x_max - a->x_min);
    float ub = (b->y_max - b->y_min) * (b->x_max - b->x_min);
    float denom = ua + ub - inter;
    return denom > 0 ? inter / denom : 0.0f;
}

static int nms(FaceBox *boxes, int n, float iou_thr)
{
    for (int i = 1; i < n; i++) {
        FaceBox key = boxes[i]; int j = i - 1;
        while (j >= 0 && boxes[j].score < key.score) { boxes[j+1]=boxes[j]; j--; }
        boxes[j+1] = key;
    }
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (boxes[i].score < 0.0f) continue;
        for (int j = i+1; j < n; j++) {
            if (boxes[j].score < 0.0f) continue;
            if (box_iou(&boxes[i], &boxes[j]) > iou_thr) boxes[j].score = -1.0f;
        }
        if (kept != i) boxes[kept] = boxes[i];
        kept++;
    }
    return kept;
}

/* -------------------------------------------------------------------------
 * ONNX session init / destroy — YOLO
 * ------------------------------------------------------------------------- */
static void onnx_init_yolo(CamAiContext *cam, OrtEnv *env)
{
    cam->ort_ready = false;
    if (!env) return;

    OrtSessionOptions *sopts = NULL;
    if (g_ort->CreateSessionOptions(&sopts) != NULL) return;
    g_ort->SetIntraOpNumThreads(sopts, 1);
    g_ort->SetSessionGraphOptimizationLevel(sopts, ORT_ENABLE_ALL);

    OrtStatus *st = g_ort->CreateSession(env, cam->face_model_path, sopts,
                                          &cam->ort_session);
    g_ort->ReleaseSessionOptions(sopts);

    if (st != NULL) {
        const char *msg = g_ort->GetErrorMessage(st);
        LOG_WARN(cam->ctx, "AI",
                 "[%s] ORT YOLO: model not loaded (%s) — face detection disabled",
                 cam->camera_name, msg);
        g_ort->ReleaseStatus(st);
        cam->ort_session = NULL;
        return;
    }

    cam->ort_ready = true;
    LOG_INFO(cam->ctx, "AI",
             "[%s] ONNX Runtime: YOLOv8-face loaded (%s)",
             cam->camera_name, cam->face_model_path);
}

static void onnx_destroy_yolo(CamAiContext *cam)
{
    if (cam->ort_session) {
        g_ort->ReleaseSession(cam->ort_session);
        cam->ort_session = NULL;
    }
    cam->ort_ready = false;
}

/* -------------------------------------------------------------------------
 * ONNX session init / destroy — MobileFaceNet embedding
 * ------------------------------------------------------------------------- */
static void onnx_init_embed(CamAiContext *cam, OrtEnv *env)
{
    cam->embed_ready = false;
    if (!env || !cam->embed_model_path[0]) return;

    OrtSessionOptions *sopts = NULL;
    if (g_ort->CreateSessionOptions(&sopts) != NULL) return;
    g_ort->SetIntraOpNumThreads(sopts, 1);
    g_ort->SetSessionGraphOptimizationLevel(sopts, ORT_ENABLE_ALL);

    OrtStatus *st = g_ort->CreateSession(env, cam->embed_model_path, sopts,
                                          &cam->embed_session);
    g_ort->ReleaseSessionOptions(sopts);

    if (st != NULL) {
        const char *msg = g_ort->GetErrorMessage(st);
        LOG_WARN(cam->ctx, "AI",
                 "[%s] ORT Embed: model not loaded (%s) — embedding disabled",
                 cam->camera_name, msg);
        g_ort->ReleaseStatus(st);
        cam->embed_session = NULL;
        return;
    }

    cam->embed_ready = true;
    LOG_INFO(cam->ctx, "AI",
             "[%s] ONNX Runtime: MobileFaceNet loaded (%s)",
             cam->camera_name, cam->embed_model_path);
}

static void onnx_destroy_embed(CamAiContext *cam)
{
    if (cam->embed_session) {
        g_ort->ReleaseSession(cam->embed_session);
        cam->embed_session = NULL;
    }
    cam->embed_ready = false;
}

/* Combined init / destroy */
static void onnx_init(CamAiContext *cam, OrtEnv *env)
{
    onnx_init_yolo(cam, env);
    onnx_init_embed(cam, env);
}

static void onnx_destroy(CamAiContext *cam)
{
    onnx_destroy_yolo(cam);
    onnx_destroy_embed(cam);
}

/* -------------------------------------------------------------------------
 * Decode YOLOv8 output [1, 5, num_anchors] -> FaceBox list + NMS
 * ------------------------------------------------------------------------- */
static int yolo_decode_output(
        const float *data,
        int          num_anchors,
        float        score_thr,
        int          fw, int fh,
        float        scale,
        float        pad_x, float pad_y,
        FaceBox     *out_boxes, int out_cap)
{
    int count = 0;

    /* YOLOv8-face output layout per anchor:
     *   row 0: cx,  row 1: cy,  row 2: w,   row 3: h   (640px space)
     *   row 4: confidence
     *   rows 5..19: 5 keypoints × (kx, ky, visibility) — present when the
     *               model was exported with keypoints (yolov8n-face.onnx typically is).
     *   If the model has only 5 rows (no keypoints), have_landmarks stays false.
     */
    int has_kpts = 0; /* will be set to 1 if output has >= 20 rows */
    /* We infer the number of rows from the anchor count and total data size.
     * Since we only have a pointer we use num_rows passed from the caller;
     * as a safe default, check at first decode. */
    /* Note: caller passes the actual dims[1] (rows) via a local variable below.
     * For backward compat with the existing call site signature we detect
     * whether num_anchors > 8400 (which would be impossible) and treat it
     * as an encoded (num_rows << 16 | num_anchors) pack — see onnx_detect_faces. */
    int num_rows = 5;
    if (num_anchors & 0xFFFF0000) {
        num_rows    = (num_anchors >> 16) & 0xFFFF;
        num_anchors =  num_anchors        & 0xFFFF;
    }
    has_kpts = (num_rows >= 20) ? 1 : 0;

    for (int i = 0; i < num_anchors && count < out_cap; i++) {
        float conf = data[4 * num_anchors + i];
        if (conf < score_thr) continue;

        float cx = data[0 * num_anchors + i];
        float cy = data[1 * num_anchors + i];
        float bw = data[2 * num_anchors + i];
        float bh = data[3 * num_anchors + i];

        float fx_min = ((cx - bw * 0.5f) - pad_x) / scale / (float)fw;
        float fy_min = ((cy - bh * 0.5f) - pad_y) / scale / (float)fh;
        float fx_max = ((cx + bw * 0.5f) - pad_x) / scale / (float)fw;
        float fy_max = ((cy + bh * 0.5f) - pad_y) / scale / (float)fh;

        FaceBox b;
        b.x_min = fx_min < 0.0f ? 0.0f : fx_min > 1.0f ? 1.0f : fx_min;
        b.y_min = fy_min < 0.0f ? 0.0f : fy_min > 1.0f ? 1.0f : fy_min;
        b.x_max = fx_max < 0.0f ? 0.0f : fx_max > 1.0f ? 1.0f : fx_max;
        b.y_max = fy_max < 0.0f ? 0.0f : fy_max > 1.0f ? 1.0f : fy_max;
        b.score = conf;

        /* Extract 5-point landmarks if present.
         * YOLOv8-face keypoint layout (15 values starting at row 5):
         *   kpt_k_x = data[(5 + k*3 + 0) * num_anchors + i]  (640px space)
         *   kpt_k_y = data[(5 + k*3 + 1) * num_anchors + i]
         *   kpt_k_v = data[(5 + k*3 + 2) * num_anchors + i]  (visibility, ignored)
         * We de-letterbox and normalise to [0,1] frame space. */
        b.lm.have_landmarks = false;
        if (has_kpts) {
            b.lm.have_landmarks = true;
            for (int k = 0; k < 5; k++) {
                float kpx = data[(5 + k*3 + 0) * num_anchors + i];
                float kpy = data[(5 + k*3 + 1) * num_anchors + i];
                b.lm.kx[k] = ((kpx - pad_x) / scale) / (float)fw;
                b.lm.ky[k] = ((kpy - pad_y) / scale) / (float)fh;
                /* Clamp */
                if (b.lm.kx[k] < 0.0f) b.lm.kx[k] = 0.0f;
                if (b.lm.kx[k] > 1.0f) b.lm.kx[k] = 1.0f;
                if (b.lm.ky[k] < 0.0f) b.lm.ky[k] = 0.0f;
                if (b.lm.ky[k] > 1.0f) b.lm.ky[k] = 1.0f;
            }
        }

        out_boxes[count++] = b;
    }
    return nms(out_boxes, count, FACE_IOU_THRESHOLD);
}

/* -------------------------------------------------------------------------
 * Run YOLO inference on the full frame.
 * Returns number of detected faces.
 * ------------------------------------------------------------------------- */
static int onnx_detect_faces(CamAiContext *cam,
                              const uint8_t *y, const uint8_t *u, const uint8_t *v,
                              int fw, int fh,
                              FaceBox *out_boxes, int out_cap)
{
    if (!cam->ort_ready || !cam->ort_session) return 0;

    float scale, pad_x, pad_y;
    yuv_to_yolo_input(y, u, v, fw, fh,
                      cam->yolo_input_buf, &scale, &pad_x, &pad_y);

    OrtMemoryInfo *mem_info = NULL;
    g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

    int64_t input_shape[] = { 1, YOLO_INPUT_CHANNELS, YOLO_INPUT_H, YOLO_INPUT_W };
    size_t  input_bytes   = (size_t)(YOLO_INPUT_CHANNELS * YOLO_INPUT_H * YOLO_INPUT_W)
                            * sizeof(float);

    OrtValue *input_tensor = NULL;
    OrtStatus *st = g_ort->CreateTensorWithDataAsOrtValue(
            mem_info,
            cam->yolo_input_buf, input_bytes,
            input_shape, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &input_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);
    if (st != NULL) { g_ort->ReleaseStatus(st); return 0; }

    const char *input_names[]  = { YOLO_INPUT_NAME  };
    const char *output_names[] = { YOLO_OUTPUT_NAME };
    OrtValue *output_tensor = NULL;

    st = g_ort->Run(cam->ort_session, NULL,
                    input_names,  (const OrtValue *const *)&input_tensor, 1,
                    output_names, 1,
                    &output_tensor);
    g_ort->ReleaseValue(input_tensor);

    if (st != NULL) {
        const char *msg = g_ort->GetErrorMessage(st);
        LOG_WARN(cam->ctx, "AI", "[%s] ORT YOLO Run failed: %s", cam->camera_name, msg);
        g_ort->ReleaseStatus(st);
        return 0;
    }

    float *output_data = NULL;
    g_ort->GetTensorMutableData(output_tensor, (void **)&output_data);

    OrtTensorTypeAndShapeInfo *shape_info = NULL;
    g_ort->GetTensorTypeAndShape(output_tensor, &shape_info);
    size_t ndim = 0;
    g_ort->GetDimensionsCount(shape_info, &ndim);
    int64_t dims[4] = {0};
    if (ndim >= 3) g_ort->GetDimensions(shape_info, dims, ndim < 4 ? ndim : 4);
    g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);

    int num_anchors = (ndim >= 3) ? (int)dims[2] : 8400;
    int num_rows    = (ndim >= 3) ? (int)dims[1] : 5;

    /* Pack num_rows into the high 16 bits of the encoded value so the
     * decoder can detect whether keypoints are present without adding a
     * new function parameter.  The decoder unpacks this immediately. */
    int encoded = (num_rows << 16) | (num_anchors & 0xFFFF);

    int nfaces = yolo_decode_output(output_data, encoded,
                                    FACE_SCORE_THRESHOLD,
                                    fw, fh, scale, pad_x, pad_y,
                                    out_boxes, out_cap);
    g_ort->ReleaseValue(output_tensor);
    return nfaces;
}

/* -------------------------------------------------------------------------
 * Run MobileFaceNet embedding on a single face crop.
 *
 * Fills `embedding[EMBED_DIM]` with a L2-normalised 128-float vector.
 * Returns true on success, false if embedding session is unavailable or fails.
 * ------------------------------------------------------------------------- */
static bool onnx_embed_face(CamAiContext *cam,
                             const uint8_t *y, const uint8_t *u, const uint8_t *v,
                             int fw, int fh,
                             float y_min, float x_min, float y_max, float x_max,
                             const FaceLandmarks *lm,
                             float *embedding)
{
    if (!cam->embed_ready || !cam->embed_session) return false;

    /* Preprocess: aligned crop -> embed_input_buf (NCHW, [-1,1])
     *
     * If 5-point landmarks are available: compute a similarity transform to
     * the ArcFace canonical 112×112 pose and warp with bilinear sampling.
     * This is the single biggest quality lever — aligned faces produce
     * cosine similarities of 0.7-0.9 for the same person vs. 0.3-0.5
     * without alignment.
     *
     * If landmarks are absent: fall back to the existing nearest-neighbour
     * bbox crop so there is zero regression on models that lack keypoints.
     */
    if (lm && lm->have_landmarks) {
        /* Convert normalised landmark coords -> pixel space */
        float src_px[5], src_py[5];
        for (int k = 0; k < 5; k++) {
            src_px[k] = lm->kx[k] * (float)fw;
            src_py[k] = lm->ky[k] * (float)fh;
        }
        float sim_m[6];
        solve_similarity_transform_2x3(src_px, src_py,
                                        kRefLmX, kRefLmY,
                                        sim_m);
        yuv_aligned_to_embed_input(y, u, v, fw, fh,
                                   sim_m,
                                   cam->embed_input_buf);
    } else {
        yuv_crop_to_embed_input(y, u, v, fw, fh,
                                y_min, x_min, y_max, x_max,
                                cam->embed_input_buf);
    }

    OrtMemoryInfo *mem_info = NULL;
    g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

    int64_t input_shape[] = { 1, EMBED_INPUT_CHANNELS, EMBED_INPUT_H, EMBED_INPUT_W };
    size_t  input_bytes   = (size_t)(EMBED_INPUT_CHANNELS * EMBED_INPUT_H * EMBED_INPUT_W)
                            * sizeof(float);

    OrtValue *input_tensor = NULL;
    OrtStatus *st = g_ort->CreateTensorWithDataAsOrtValue(
            mem_info,
            cam->embed_input_buf, input_bytes,
            input_shape, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &input_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);
    if (st != NULL) { g_ort->ReleaseStatus(st); return false; }

    const char *input_names[]  = { EMBED_INPUT_NAME  };
    const char *output_names[] = { EMBED_OUTPUT_NAME };
    OrtValue *output_tensor = NULL;

    st = g_ort->Run(cam->embed_session, NULL,
                    input_names,  (const OrtValue *const *)&input_tensor, 1,
                    output_names, 1,
                    &output_tensor);
    g_ort->ReleaseValue(input_tensor);

    if (st != NULL) {
        const char *msg = g_ort->GetErrorMessage(st);
        LOG_WARN(cam->ctx, "AI", "[%s] ORT Embed Run failed: %s",
                 cam->camera_name, msg);
        g_ort->ReleaseStatus(st);
        return false;
    }

    /* Copy output vector */
    float *output_data = NULL;
    g_ort->GetTensorMutableData(output_tensor, (void **)&output_data);

    /* Verify output has at least EMBED_DIM elements */
    OrtTensorTypeAndShapeInfo *si = NULL;
    g_ort->GetTensorTypeAndShape(output_tensor, &si);
    size_t ndim = 0; int64_t dims[4] = {0};
    g_ort->GetDimensionsCount(si, &ndim);
    if (ndim >= 2) g_ort->GetDimensions(si, dims, ndim < 4 ? ndim : 4);
    g_ort->ReleaseTensorTypeAndShapeInfo(si);

    int out_dim = (ndim >= 2) ? (int)dims[1] : EMBED_DIM;
    if (out_dim < EMBED_DIM) {
        LOG_WARN(cam->ctx, "AI",
                 "[%s] Embed output dim %d < expected %d",
                 cam->camera_name, out_dim, EMBED_DIM);
        g_ort->ReleaseValue(output_tensor);
        return false;
    }

    memcpy(embedding, output_data, EMBED_DIM * sizeof(float));
    g_ort->ReleaseValue(output_tensor);

    /* L2-normalise (model may or may not have done this internally) */
    l2_normalize(embedding, EMBED_DIM);
    return true;
}

#else /* !MNVR_WITH_ONNX */

static void onnx_init(CamAiContext *cam, void *env)    { (void)cam; (void)env; }
static void onnx_destroy(CamAiContext *cam)            { (void)cam; }
static int  onnx_detect_faces(CamAiContext *cam,
        const uint8_t *y, const uint8_t *u, const uint8_t *v,
        int fw, int fh, FaceBox *boxes, int cap)
{
    (void)cam;(void)y;(void)u;(void)v;(void)fw;(void)fh;(void)boxes;(void)cap;
    return 0;
}
static bool onnx_embed_face(CamAiContext *cam,
        const uint8_t *y, const uint8_t *u, const uint8_t *v,
        int fw, int fh,
        float y_min, float x_min, float y_max, float x_max,
        const FaceLandmarks *lm,
        float *embedding)
{
    (void)cam;(void)y;(void)u;(void)v;(void)fw;(void)fh;
    (void)y_min;(void)x_min;(void)y_max;(void)x_max;(void)lm;(void)embedding;
    return false;
}

#endif /* MNVR_WITH_ONNX */

/* =========================================================================
 * Frame processor  (runs in AI worker thread)
 * ========================================================================= */

static void process_frame(CamAiContext *cam, AiFrame *frame)
{
    int w = frame->width;
    int h = frame->height;

    cam->face_frame_counter++;
    if (cam->face_frame_counter % 200 == 1) {
        LOG_INFO(cam->ctx, "AI", "[%s] Frame #%d (%dx%d)",
                 cam->camera_name, cam->face_frame_counter, w, h);
    }

    /* --- Motion detection on central ROI --- */
    if (cam->prev_frame && cam->prev_width == w && cam->prev_height == h) {
        float motion = compute_motion_roi(frame->y_plane, cam->prev_frame,
                                          w, h, MOTION_ROI_FRACTION);
        if (motion > cam->motion_threshold) {
            LOG_INFO(cam->ctx, "AI", "[%s] Motion %.2f%% (ROI)",
                     cam->camera_name, motion * 100.0f);
            if (cam->on_event) {
                AiEvent ev = {
                    .camera_id  = cam->camera_id,
                    .type       = AI_EVENT_MOTION,
                    .confidence = motion,
                    .pts_ms     = frame->pts_ms,
                };
                snprintf(ev.metadata, sizeof(ev.metadata),
                         "{\"motion_ratio\":%.4f,\"roi\":%.2f}",
                         motion, MOTION_ROI_FRACTION);
                cam->on_event(&ev, cam->event_user_data);
            }
            if (cam->ctx && cam->ctx->db)
                db_insert_motion_event(cam->ctx->db, cam->camera_id,
                                       motion, frame->pts_ms);
        }
    }

    if (!cam->prev_frame || cam->prev_width != w || cam->prev_height != h) {
        free(cam->prev_frame);
        cam->prev_frame  = malloc((size_t)w * (size_t)h);
        cam->prev_width  = w;
        cam->prev_height = h;
    }
    if (cam->prev_frame)
        memcpy(cam->prev_frame, frame->y_plane, (size_t)w * (size_t)h);

    /* --- YOLO face detection (every face_detect_interval frames) --- */
    if (cam->enable_face) {
        static __thread int face_tick = 0;
        face_tick++;
        if (face_tick >= cam->face_detect_interval) {
            face_tick = 0;

            FaceBox boxes[FACE_MAX_DETECTIONS];
            int nfaces = onnx_detect_faces(cam,
                            frame->y_plane, frame->u_plane, frame->v_plane,
                            w, h, boxes, FACE_MAX_DETECTIONS);

            if (nfaces > 0) {
                float top_score = boxes[0].score;

                char bbox_json[512];
                int  blen = snprintf(bbox_json, sizeof(bbox_json), "[");
                for (int i = 0; i < nfaces && blen < (int)sizeof(bbox_json)-60; i++) {
                    if (i > 0)
                        blen += snprintf(bbox_json+blen, sizeof(bbox_json)-blen, ",");
                    blen += snprintf(bbox_json+blen, sizeof(bbox_json)-blen,
                                     "{\"y\":%.3f,\"x\":%.3f,"
                                     "\"h\":%.3f,\"w\":%.3f,\"s\":%.3f}",
                                     boxes[i].y_min, boxes[i].x_min,
                                     boxes[i].y_max - boxes[i].y_min,
                                     boxes[i].x_max - boxes[i].x_min,
                                     boxes[i].score);
                }
                snprintf(bbox_json+blen, sizeof(bbox_json)-blen, "]");

                int unique_count = 0;

                if (cam->ctx && cam->ctx->db) {
                    for (int i = 0; i < nfaces; i++) {

                        /* --- Step 0: temporal bbox cooldown ---
                         *
                         * A face standing still in frame re-enters every
                         * face_detect_interval frames.  If the bbox centre is
                         * within TEMPORAL_COOLDOWN_DIST of a recently-processed
                         * detection and within TEMPORAL_COOLDOWN_MS, skip all
                         * inference + DB work for this box.  This eliminates
                         * the "same face 0.33 / 0.42 / 0.50 flutter" caused by
                         * feeding near-identical crops repeatedly.
                         */
                        float cx = (boxes[i].x_min + boxes[i].x_max) * 0.5f;
                        float cy = (boxes[i].y_min + boxes[i].y_max) * 0.5f;
                        if (temporal_cooldown_check(&cam->temporal_cooldown,
                                                    cx, cy, frame->pts_ms)) {
                            /* Still hot — skip silently */
                            continue;
                        }

                        /* --- Step 1: compute MobileFaceNet embedding ---
                         *
                         * Pass landmarks so the embedding function can apply
                         * affine alignment (eye-centre warp to 112×112 canonical
                         * pose) before running MobileFaceNet.  This raises
                         * same-person cosine similarity from ~0.35-0.50 to
                         * ~0.70-0.90, making the 0.50 threshold reliable.
                         */
                        float embedding[EMBED_DIM];
                        bool  have_embedding = onnx_embed_face(
                                cam,
                                frame->y_plane, frame->u_plane, frame->v_plane,
                                w, h,
                                boxes[i].y_min, boxes[i].x_min,
                                boxes[i].y_max, boxes[i].x_max,
                                &boxes[i].lm,
                                embedding);

                        /* --- Step 2: cosine-similarity dedup ---
                         *
                         * If embedding is available, use it.
                         * Falls back to "always unique" if embedding session
                         * is not loaded (model file missing / ONNX not built).
                         */
                        int  face_is_unique = 1;

                        if (have_embedding) {
                            int match = embed_dedup_check_and_insert(
                                    &cam->embed_dedup, embedding);
                            face_is_unique = (match < 0) ? 1 : 0;
                        }

                        /* --- Step 3: build face path --- */
                        char face_path[MNVR_MAX_PATH];
                        snprintf(face_path, sizeof(face_path),
                                "/storage/faces/cam%d_%llu_f%d.jpg",
                                cam->camera_id,
                                (unsigned long long)frame->pts_ms, i);

                        /* --- Step 4: write to DB (with embedding bytes) --- */
                        int face_id = db_insert_face_with_dedup_embed(
                                cam->ctx->db, cam->camera_id,
                                boxes[i].score,
                                boxes[i].y_min, boxes[i].x_min,
                                boxes[i].y_max, boxes[i].x_max,
                                w, h, frame->pts_ms,
                                face_path,
                                30,              /* dedup_window_minutes */
                                face_is_unique,
                                have_embedding ? embedding : NULL,
                                EMBED_DIM);

                        /* --- Step 5: save JPEG only for unique faces --- */
                        if (face_is_unique) {
                            int saved = save_face_crop_colour(
                                    frame->y_plane, frame->u_plane, frame->v_plane,
                                    w, h,
                                    boxes[i].y_min, boxes[i].x_min,
                                    boxes[i].y_max, boxes[i].x_max,
                                    face_path);
                            if (saved) {
                                unique_count++;
                                LOG_INFO(cam->ctx, "AI",
                                        "[%s] Face %d/%d -> UNIQUE saved: %s "
                                        "(conf=%.2f embed=%s)",
                                        cam->camera_name, i+1, nfaces,
                                        face_path, boxes[i].score,
                                        have_embedding ? "yes" : "no");
                            }
                        } else {
                            LOG_INFO(cam->ctx, "AI",
                                    "[%s] Face %d/%d -> DUPLICATE skipped "
                                    "(conf=%.2f cosine>=%.2f)",
                                    cam->camera_name, i+1, nfaces,
                                    boxes[i].score, EMBED_COSINE_THRESHOLD);
                        }
                        (void)face_id;
                    }
                }

                if (cam->on_event) {
                    AiEvent ev = {
                        .camera_id  = cam->camera_id,
                        .type       = AI_EVENT_FACE_DETECTED,
                        .confidence = top_score,
                        .pts_ms     = frame->pts_ms,
                    };
                    snprintf(ev.metadata, sizeof(ev.metadata),
                             "{\"face_count\":%d,\"unique\":%d,\"boxes\":%s}",
                             nfaces, unique_count, bbox_json);
                    cam->on_event(&ev, cam->event_user_data);
                }

                LOG_INFO(cam->ctx, "AI",
                         "[%s] YOLO: %d face(s) detected (%d unique, top=%.2f)",
                         cam->camera_name, nfaces, unique_count, top_score);
            }
        }
    }

    /* --- RDAS --- */
    if (cam->enable_rdas) {
        float alert = compute_rdas(cam, frame->y_plane, w, h);
        if (alert > 0.7f && cam->on_event) {
            AiEvent ev = {
                .camera_id  = cam->camera_id,
                .type       = AI_EVENT_RDAS_ALERT,
                .confidence = alert,
                .pts_ms     = frame->pts_ms,
            };
            snprintf(ev.metadata, sizeof(ev.metadata),
                     "{\"alert_level\":%.2f}", alert);
            cam->on_event(&ev, cam->event_user_data);
        }
    }

    free(frame->y_plane); frame->y_plane = NULL;
    free(frame->u_plane); frame->u_plane = NULL;
    free(frame->v_plane); frame->v_plane = NULL;
}

/* =========================================================================
 * AI worker thread
 * ========================================================================= */

static void *ai_worker_thread(void *arg)
{
    CamAiContext *cam = (CamAiContext *)arg;
    LOG_INFO(cam->ctx, "AI",
             "[%s] AI worker started (YOLO + MobileFaceNet embed dedup)",
             cam->camera_name);

    if (cam->enable_face) {
        onnx_init(cam, ((struct AiModule *)cam->event_user_data));
    }

    while (cam->running) {
        pthread_mutex_lock(&cam->q_mutex);
        while (cam->q_tail == cam->q_head && cam->running)
            pthread_cond_wait(&cam->q_cond, &cam->q_mutex);
        if (!cam->running && cam->q_tail == cam->q_head) {
            pthread_mutex_unlock(&cam->q_mutex);
            break;
        }
        AiFrame frame = cam->queue[cam->q_tail % AI_FRAME_QUEUE_SIZE];
        cam->q_tail++;
        pthread_mutex_unlock(&cam->q_mutex);
        process_frame(cam, &frame);
    }

    while (cam->q_tail != cam->q_head) {
        AiFrame *f = &cam->queue[cam->q_tail++ % AI_FRAME_QUEUE_SIZE];
        free(f->y_plane); free(f->u_plane); free(f->v_plane);
    }

    onnx_destroy(cam);
    free(cam->prev_frame); cam->prev_frame = NULL;
    LOG_INFO(cam->ctx, "AI", "[%s] AI worker stopped", cam->camera_name);
    return NULL;
}

/* =========================================================================
 * Frame push — called from StreamerModule
 * ========================================================================= */

void ai_push_yuv_frame(const YuvFrame *frame, void *user_data)
{
    AiModule *am = (AiModule *)user_data;
    if (!am || !frame) return;

    int    w        = frame->width;
    int    h        = frame->height;
    size_t y_size   = (size_t)(w * h);
    size_t uv_size  = (size_t)((w / 2) * (h / 2));

    for (int i = 0; i < am->num_cams; i++) {
        CamAiContext *cam = &am->cams[i];
        if (cam->camera_id != frame->camera_id || !cam->running) continue;

        pthread_mutex_lock(&cam->q_mutex);

        if ((cam->q_head - cam->q_tail) >= AI_FRAME_QUEUE_SIZE) {
            AiFrame *old = &cam->queue[cam->q_tail++ % AI_FRAME_QUEUE_SIZE];
            free(old->y_plane); old->y_plane = NULL;
            free(old->u_plane); old->u_plane = NULL;
            free(old->v_plane); old->v_plane = NULL;
        }

        AiFrame *slot   = &cam->queue[cam->q_head % AI_FRAME_QUEUE_SIZE];
        slot->camera_id = frame->camera_id;
        slot->width     = w;
        slot->height    = h;
        slot->pts_ms    = frame->pts_ms;

        slot->y_plane = malloc(y_size);
        slot->u_plane = malloc(uv_size);
        slot->v_plane = malloc(uv_size);

        if (slot->y_plane && slot->u_plane && slot->v_plane) {
            memcpy(slot->y_plane, frame->y, y_size);
            memcpy(slot->u_plane, frame->u, uv_size);
            memcpy(slot->v_plane, frame->v, uv_size);
            cam->q_head++;
            pthread_cond_signal(&cam->q_cond);
        } else {
            free(slot->y_plane); slot->y_plane = NULL;
            free(slot->u_plane); slot->u_plane = NULL;
            free(slot->v_plane); slot->v_plane = NULL;
        }

        pthread_mutex_unlock(&cam->q_mutex);
        return;
    }
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

AiModule *ai_module_create(AppContext *ctx, OnAiEvent cb, void *user_data)
{
    AiModule *am = calloc(1, sizeof(AiModule));
    if (!am) return NULL;
    am->ctx             = ctx;
    am->on_event        = cb;
    am->event_user_data = user_data;
    pthread_mutex_init(&am->mutex, NULL);

#ifdef MNVR_WITH_ONNX
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        LOG_ERROR(ctx, "AI", "ORT: failed to get API base — face/embed disabled");
    } else {
        OrtStatus *st = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                          "mnvr_ai", &am->ort_env);
        if (st) {
            g_ort->ReleaseStatus(st);
            am->ort_env = NULL;
            LOG_ERROR(ctx, "AI", "ORT: CreateEnv failed — face/embed disabled");
        }
    }
#endif

    return am;
}

MnvrResult ai_module_start(AiModule *am)
{
    if (!am || !am->ctx) return MNVR_ERR_GENERIC;
    AppContext         *ctx = am->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    const char *model_path = config_get_str(ctx->config, "face_model_path",
                                             "/etc/mnvr/yolov8n-face.onnx");
    const char *embed_path = config_get_str(ctx->config, "embed_model_path",
                                             "/etc/mnvr/mobilefacenet.onnx");
    int face_interval = config_get_int(ctx->config, "face_detect_interval",
                                        FACE_DETECT_DEFAULT_INTERVAL);
    if (face_interval < 1)   face_interval = 1;
    if (face_interval > 120) face_interval = 120;

    float motion_thr = cfg ? cfg->motion_threshold : MOTION_DEFAULT_THRESHOLD;
    if (motion_thr > 0.03f) motion_thr = MOTION_DEFAULT_THRESHOLD;

    am->num_cams = 0;
    for (int i = 0; i < ctx->num_cameras; i++) {
        CameraInfo   *info = &ctx->cameras[i];
        CamAiContext *cam  = &am->cams[am->num_cams];
        memset(cam, 0, sizeof(CamAiContext));

        cam->ctx              = ctx;
        cam->camera_id        = info->camera_id;
        cam->on_event         = am->on_event;
        cam->event_user_data  = am;
        cam->motion_threshold = motion_thr;
        cam->enable_face      = cfg ? cfg->enable_face_detection : true;
        cam->enable_rdas      = cfg ? cfg->enable_rdas : false;
        cam->face_detect_interval = face_interval;
        strncpy(cam->camera_name,     info->name,    MNVR_MAX_NAME - 1);
        strncpy(cam->face_model_path, model_path,    MNVR_MAX_PATH - 1);
        strncpy(cam->embed_model_path, embed_path,   MNVR_MAX_PATH - 1);

        if (cam->enable_face) {
            /* YOLO input buffer */
            cam->yolo_input_buf = malloc(
                    (size_t)(YOLO_INPUT_W * YOLO_INPUT_H *
                             YOLO_INPUT_CHANNELS) * sizeof(float));
            if (!cam->yolo_input_buf) {
                LOG_WARN(ctx, "AI",
                         "[%s] yolo_input_buf alloc failed — face detection disabled",
                         cam->camera_name);
                cam->enable_face = false;
                goto skip_embed_alloc;
            }

            /* MobileFaceNet input buffer */
            cam->embed_input_buf = malloc(
                    (size_t)(EMBED_INPUT_W * EMBED_INPUT_H *
                             EMBED_INPUT_CHANNELS) * sizeof(float));
            if (!cam->embed_input_buf) {
                LOG_WARN(ctx, "AI",
                         "[%s] embed_input_buf alloc failed — embedding disabled "
                         "(dedup will fall back to time-window only)",
                         cam->camera_name);
                /* Not fatal: embedding is optional, YOLO still runs */
            }
        }
skip_embed_alloc:;

        /* Initialise embedding dedup table and temporal cooldown */
        memset(&cam->embed_dedup,       0, sizeof(EmbedDedupTable));
        memset(&cam->temporal_cooldown, 0, sizeof(TemporalCooldownTable));

        pthread_mutex_init(&cam->q_mutex, NULL);
        pthread_cond_init(&cam->q_cond, NULL);
        cam->running = true;

        if (pthread_create(&cam->thread, NULL, ai_worker_thread, cam) != 0) {
            LOG_ERROR(ctx, "AI", "Failed to start AI worker for cam %d",
                      info->camera_id);
            cam->running = false;
            free(cam->yolo_input_buf);  cam->yolo_input_buf  = NULL;
            free(cam->embed_input_buf); cam->embed_input_buf = NULL;
            continue;
        }
        am->num_cams++;
    }

#ifdef MNVR_WITH_ONNX
    LOG_INFO(ctx, "AI",
             "Started %d AI worker(s) — YOLO face detection ENABLED "
             "(model: %s, embed: %s, interval: every %d frames, "
             "cosine threshold: %.2f, dedup window: %d groups)",
             am->num_cams, model_path, embed_path, face_interval,
             EMBED_COSINE_THRESHOLD, EMBED_DEDUP_WINDOW);
#else
    LOG_INFO(ctx, "AI",
             "Started %d AI worker(s) — ONNX Runtime not compiled in, "
             "face detection + embedding DISABLED (build with -DMNVR_WITH_ONNX)",
             am->num_cams);
#endif

    return MNVR_OK;
}

void ai_module_stop(AiModule *am)
{
    if (!am) return;
    for (int i = 0; i < am->num_cams; i++) {
        CamAiContext *cam = &am->cams[i];
        pthread_mutex_lock(&cam->q_mutex);
        cam->running = false;
        pthread_cond_signal(&cam->q_cond);
        pthread_mutex_unlock(&cam->q_mutex);
        pthread_join(cam->thread, NULL);
        pthread_mutex_destroy(&cam->q_mutex);
        pthread_cond_destroy(&cam->q_cond);
        free(cam->yolo_input_buf);  cam->yolo_input_buf  = NULL;
        free(cam->embed_input_buf); cam->embed_input_buf = NULL;
    }
    LOG_INFO(am->ctx, "AI", "All AI workers stopped");
}

void ai_module_destroy(AiModule *am)
{
    if (!am) return;
    ai_module_stop(am);

#ifdef MNVR_WITH_ONNX
    if (g_ort && am->ort_env) {
        g_ort->ReleaseEnv(am->ort_env);
        am->ort_env = NULL;
    }
#endif

    pthread_mutex_destroy(&am->mutex);
    free(am);
}

/* -------------------------------------------------------------------------
 * Hot-add a camera at runtime (mirrors the setup in ai_module_start)
 * ------------------------------------------------------------------------- */
MnvrResult ai_add_camera(AiModule *am, const CameraInfo *info)
{
    if (!am || !info || !am->ctx) return MNVR_ERR_GENERIC;

    /* Already have a worker for this camera? */
    for (int i = 0; i < am->num_cams; i++) {
        if (am->cams[i].camera_id == info->camera_id)
            return MNVR_OK;
    }

    if (am->num_cams >= MNVR_MAX_CAMERAS)
        return MNVR_ERR_BUSY;

    AppContext         *ctx = am->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    const char *model_path = config_get_str(ctx->config, "face_model_path",
                                             "/etc/mnvr/yolov8n-face.onnx");
    const char *embed_path = config_get_str(ctx->config, "embed_model_path",
                                             "/etc/mnvr/mobilefacenet.onnx");
    int face_interval = config_get_int(ctx->config, "face_detect_interval",
                                        FACE_DETECT_DEFAULT_INTERVAL);
    if (face_interval < 1)   face_interval = 1;
    if (face_interval > 120) face_interval = 120;

    float motion_thr = cfg ? cfg->motion_threshold : MOTION_DEFAULT_THRESHOLD;
    if (motion_thr > 0.03f) motion_thr = MOTION_DEFAULT_THRESHOLD;

    pthread_mutex_lock(&am->mutex);

    CamAiContext *cam = &am->cams[am->num_cams];
    memset(cam, 0, sizeof(CamAiContext));

    cam->ctx              = ctx;
    cam->camera_id        = info->camera_id;
    cam->on_event         = am->on_event;
    cam->event_user_data  = am;
    cam->motion_threshold = motion_thr;
    cam->enable_face      = cfg ? cfg->enable_face_detection : true;
    cam->enable_rdas      = cfg ? cfg->enable_rdas : false;
    cam->face_detect_interval = face_interval;
    strncpy(cam->camera_name,      info->name,   MNVR_MAX_NAME - 1);
    strncpy(cam->face_model_path,  model_path,   MNVR_MAX_PATH - 1);
    strncpy(cam->embed_model_path, embed_path,   MNVR_MAX_PATH - 1);

    if (cam->enable_face) {
        cam->yolo_input_buf = malloc(
                (size_t)(YOLO_INPUT_W * YOLO_INPUT_H *
                         YOLO_INPUT_CHANNELS) * sizeof(float));
        if (!cam->yolo_input_buf) {
            LOG_WARN(ctx, "AI",
                     "[%s] yolo_input_buf alloc failed — face detection disabled (hot-add)",
                     cam->camera_name);
            cam->enable_face = false;
        } else {
            cam->embed_input_buf = malloc(
                    (size_t)(EMBED_INPUT_W * EMBED_INPUT_H *
                             EMBED_INPUT_CHANNELS) * sizeof(float));
            if (!cam->embed_input_buf) {
                LOG_WARN(ctx, "AI",
                         "[%s] embed_input_buf alloc failed — embedding disabled (hot-add)",
                         cam->camera_name);
            }
        }
    }

    memset(&cam->embed_dedup,       0, sizeof(EmbedDedupTable));
    memset(&cam->temporal_cooldown, 0, sizeof(TemporalCooldownTable));

    pthread_mutex_init(&cam->q_mutex, NULL);
    pthread_cond_init(&cam->q_cond, NULL);
    cam->running = true;

    if (pthread_create(&cam->thread, NULL, ai_worker_thread, cam) != 0) {
        LOG_ERROR(ctx, "AI", "[cam %d] Failed to start AI worker (hot-add)",
                  info->camera_id);
        cam->running = false;
        free(cam->yolo_input_buf);  cam->yolo_input_buf  = NULL;
        free(cam->embed_input_buf); cam->embed_input_buf = NULL;
        pthread_mutex_unlock(&am->mutex);
        return MNVR_ERR_GENERIC;
    }

    am->num_cams++;
    pthread_mutex_unlock(&am->mutex);

    LOG_INFO(ctx, "AI", "[cam %d] AI worker hot-added (%s)",
             info->camera_id, cam->camera_name);
    return MNVR_OK;
}