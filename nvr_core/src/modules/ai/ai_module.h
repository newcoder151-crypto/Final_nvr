/**
 * @file ai_module.h
 * @brief AI analytics module: motion detection, face detection (YOLO), RDAS,
 *        and unique-face deduplication via MobileFaceNet embedding + cosine similarity.
 *
 * Receives raw I420 frames via ai_push_yuv_frame() from StreamerModule.
 * Runs analytics in a per-camera worker thread (frame queue).
 *
 * Features:
 *   - Motion detection  (frame-diff on luma, configurable threshold, central ROI)
 *   - Face detection    (YOLOv8-face ONNX model via ONNX Runtime C API)
 *   - Face embedding    (MobileFaceNet ONNX model, 128-dim float vector)
 *   - Unique-face dedup (cosine similarity on embeddings, threshold 0.5)
 *   - RDAS              (Driver Alertness System - eye-blink / yawn stub)
 *
 * YOLO face detection:
 *   Uses a YOLOv8n-face ONNX model (640x640 input).
 *   Input  : [1, 3, 640, 640] float32, normalised to [0, 1] in RGB order.
 *   Output : [1, 5, num_anchors] -- (cx, cy, w, h, conf) per anchor.
 *   Post:    confidence threshold -> decode boxes -> NMS.
 *   Model path: config key  face_model_path
 *               (default: /etc/mnvr/yolov8n-face.onnx)
 *
 * MobileFaceNet embedding:
 *   Uses a MobileFaceNet ONNX model (~2MB, fast on CPU).
 *   Input  : [1, 3, 112, 112] float32, normalised to [-1, 1] in RGB order.
 *   Output : [1, 128] float32 L2-normalised embedding vector.
 *   Cosine similarity >= EMBED_COSINE_THRESHOLD -> same person (duplicate).
 *   Model path: config key  embed_model_path
 *               (default: /etc/mnvr/mobilefacenet.onnx)
 *
 *   Download (example):
 *     wget -O /etc/mnvr/mobilefacenet.onnx \
 *       https://github.com/onnx/models/raw/main/validated/vision/body_analysis/arcface/model/arcfaceresnet100-8.onnx
 *   Or use a pre-exported MobileFaceNet:
 *     https://github.com/sirius-ai/MobileFaceNet_TF (export to ONNX with input 112x112)
 *
 * Deduplication pipeline:
 *   YOLO detects face -> crop -> MobileFaceNet -> 128-dim embedding ->
 *   cosine similarity vs. per-camera group representative embeddings ->
 *   similarity >= threshold  -> duplicate (is_unique=0)
 *   similarity <  threshold  -> new group  (is_unique=1)
 *
 * Build flags:
 *   Compile with  -DMNVR_WITH_ONNX  and link  -lonnxruntime
 *   The Makefile auto-detects via pkg-config onnxruntime.
 */

#ifndef AI_MODULE_H
#define AI_MODULE_H

#include "mnvr_system.h"
#include "../streamer/streamer_module.h"

#ifdef MNVR_WITH_ONNX
#  include <onnxruntime_c_api.h>
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

#define AI_FRAME_QUEUE_SIZE         8

/* YOLO model input geometry */
#define YOLO_INPUT_W              640
#define YOLO_INPUT_H              640
#define YOLO_INPUT_CHANNELS         3

/* Detection tuning */
#define FACE_SCORE_THRESHOLD      0.45f
#define FACE_IOU_THRESHOLD        0.40f
#define FACE_MAX_DETECTIONS        32

/* Run face detector every N frames */
#define FACE_DETECT_DEFAULT_INTERVAL  6

/* Motion detection */
#define MOTION_ROI_FRACTION       0.80f
#define MOTION_DEFAULT_THRESHOLD  0.02f

/* JPEG crop */
#define FACE_CROP_PADDING         0.20f
#define FACE_CROP_JPEG_Q            85
#define FACE_CROP_MIN_PX            20

/* -------------------------------------------------------------------------
 * MobileFaceNet embedding model
 * Input: [1, 3, 112, 112]  normalised to [-1, 1]
 * Output: [1, 128]  L2-normalised embedding
 * ------------------------------------------------------------------------- */
#define EMBED_INPUT_W             112
#define EMBED_INPUT_H             112
#define EMBED_INPUT_CHANNELS        3
#define EMBED_DIM                 512 /* output vector length */

/* Cosine similarity threshold for same-person decision.
 * >= EMBED_COSINE_THRESHOLD -> duplicate.
 * Tuning guide:
 *   0.50 – lenient  (fewer false positives, may miss same person with hat/glasses)
 *   0.60 – balanced (recommended starting point)
 *   0.70 – strict   (fewer false duplicates, more unique entries) */
#define EMBED_COSINE_THRESHOLD    0.50f

/* Rolling embedding dedup table size (entries per camera).
 * Each entry = group representative embedding (128 floats = 512 bytes).
 * 64 groups * 512 B = 32 KB per camera — negligible. */
#define EMBED_DEDUP_WINDOW         64

/* =========================================================================
 * Public types
 * ========================================================================= */

typedef struct {
    uint8_t *y_plane;
    uint8_t *u_plane;
    uint8_t *v_plane;
    int      width;
    int      height;
    uint64_t pts_ms;
    int      camera_id;
} AiFrame;

typedef enum {
    AI_EVENT_MOTION        = 1,
    AI_EVENT_FACE_DETECTED = 2,
    AI_EVENT_TAMPERING     = 3,
    AI_EVENT_RDAS_ALERT    = 4,
} AiEventType;

typedef struct {
    int          camera_id;
    AiEventType  type;
    float        confidence;
    uint64_t     pts_ms;
    char         metadata[256];
} AiEvent;

typedef void (*OnAiEvent)(const AiEvent *ev, void *user_data);

/* =========================================================================
 * Face landmark struct (5-point: left-eye, right-eye, nose, mouth-left, mouth-right)
 * Used for affine alignment before MobileFaceNet embedding.
 * YOLOv8-face outputs 5 keypoints per detection; if your model does not,
 * set have_landmarks=false and alignment is skipped gracefully.
 * ========================================================================= */
typedef struct {
    float kx[5], ky[5];  /* normalised [0,1] keypoint coords (x=col, y=row) */
    bool  have_landmarks;
} FaceLandmarks;

typedef struct {
    float y_min, x_min, y_max, x_max;
    float score;
    FaceLandmarks lm;  /* 5-point landmarks from YOLOv8-face (may be absent) */
} FaceBox;

/* One group representative: a 128-dim L2-normalised embedding. */
typedef struct {
    float  vec[EMBED_DIM];
    bool   valid;           /* false = empty slot */
} EmbedEntry;

/* Rolling embedding dedup table (per camera).
 * Stores the representative embedding for each known face group.
 * New face: compute cosine sim vs. all valid entries.
 *   >= threshold -> duplicate, link to that group.
 *   <  threshold for all -> new group, insert entry.
 */
typedef struct {
    EmbedEntry entries[EMBED_DEDUP_WINDOW];
    int        count;   /* valid entries so far */
    int        next;    /* next write position (ring) */
} EmbedDedupTable;

/* =========================================================================
 * Temporal bbox cooldown table
 *
 * Suppresses redundant per-frame re-processing of the same physical face.
 * A face whose centre is within TEMPORAL_COOLDOWN_DIST of a recent entry
 * and whose timestamp is within TEMPORAL_COOLDOWN_MS is skipped entirely.
 *
 * Typical effect: a face standing in frame generates one unique event then
 * zero further inserts until it moves or the cooldown window expires.
 * ========================================================================= */

/* Maximum tracked bboxes per camera (covers FACE_MAX_DETECTIONS × a few groups) */
#define TEMPORAL_COOLDOWN_SLOTS   32
/* How long (ms) a bbox suppresses reprocessing of the same region */
#define TEMPORAL_COOLDOWN_MS    2000
/* Max normalised-coordinate centre distance to consider "same region".
 * 0.08 ≈ 8 % of frame width/height — tight enough for a stationary face,
 * loose enough to survive minor head wobble. */
#define TEMPORAL_COOLDOWN_DIST  0.08f

typedef struct {
    float    cx, cy;      /* normalised centre of last-seen bbox */
    uint64_t last_ms;     /* pts_ms when last processed */
    bool     valid;
} TemporalSlot;

typedef struct {
    TemporalSlot slots[TEMPORAL_COOLDOWN_SLOTS];
    int          next;    /* ring-buffer write head */
} TemporalCooldownTable;

/* =========================================================================
 * Per-camera AI context
 * ========================================================================= */
typedef struct {
    int          camera_id;
    char         camera_name[MNVR_MAX_NAME];

    /* Frame queue */
    AiFrame         queue[AI_FRAME_QUEUE_SIZE];
    volatile int    q_head;
    volatile int    q_tail;
    pthread_mutex_t q_mutex;
    pthread_cond_t  q_cond;

    pthread_t       thread;
    volatile bool   running;

    /* Motion */
    uint8_t    *prev_frame;
    int         prev_width;
    int         prev_height;

    /* Face detection state */
    int         face_frame_counter;

#ifdef MNVR_WITH_ONNX
    /* YOLO detection session */
    OrtSession  *ort_session;
    bool         ort_ready;

    /* MobileFaceNet embedding session */
    OrtSession  *embed_session;
    bool         embed_ready;
#endif

    /* Pre-allocated YOLO input tensor buffer */
    float *yolo_input_buf;   /* YOLO_INPUT_W * YOLO_INPUT_H * 3 floats */

    /* Pre-allocated embedding input tensor buffer */
    float *embed_input_buf;  /* EMBED_INPUT_W * EMBED_INPUT_H * 3 floats */

    /* Unique-face dedup via cosine similarity on embeddings */
    EmbedDedupTable embed_dedup;

    /* Temporal bbox cooldown — suppresses re-processing a stationary face
     * every frame.  Independent of embed_dedup; checked before inference. */
    TemporalCooldownTable temporal_cooldown;

    /* Config */
    float  motion_threshold;
    bool   enable_face;
    bool   enable_rdas;
    char   face_model_path[MNVR_MAX_PATH];
    char   embed_model_path[MNVR_MAX_PATH];
    int    face_detect_interval;

    AppContext *ctx;
    OnAiEvent  on_event;
    void      *event_user_data;
} CamAiContext;

/* =========================================================================
 * Module handle
 * ========================================================================= */
struct AiModule {
    AppContext     *ctx;
    CamAiContext    cams[MNVR_MAX_CAMERAS];
    int             num_cams;
    pthread_mutex_t mutex;
    OnAiEvent  on_event;
    void      *event_user_data;

#ifdef MNVR_WITH_ONNX
    OrtEnv    *ort_env;   /* Shared OrtEnv (one per process) */
#endif
};

/* =========================================================================
 * Public API
 * ========================================================================= */

AiModule  *ai_module_create(AppContext *ctx, OnAiEvent cb, void *user_data);
MnvrResult ai_module_start(AiModule *am);
void       ai_module_stop(AiModule *am);
void       ai_module_destroy(AiModule *am);

void ai_push_yuv_frame(const YuvFrame *frame, void *user_data);

/* ---- Hot-add a camera at runtime ---- */
MnvrResult ai_add_camera(AiModule *am, const CameraInfo *info);

#endif /* AI_MODULE_H */