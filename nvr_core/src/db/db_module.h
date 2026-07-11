/**
 * @file db_module.h
 * @brief PostgreSQL database module for mNVR
 *
 * Wraps libpq (PostgreSQL C client library) with:
 *   - Single persistent connection with automatic reconnect
 *   - Async write queue (ring buffer, non-blocking for hot paths)
 *   - Background writer thread drains the queue
 *   - Synchronous query interface for reads (API, config load)
 *   - Schema application on first run (database_pg.sql)
 *
 * Connection string format (set in mnvr.conf db_path field):
 *   "host=localhost port=5432 dbname=mnvr user=mnvr password=secret"
 *
 * All DB writes from recorder/AI hot paths use db_async_exec()
 * which enqueues SQL into a ring buffer - never blocks the caller.
 * Reads use db_query() which executes synchronously.
 *
 * Thread model:
 *   db_writer_thread: drains async write queue
 *   Any thread: db_query() - synchronous read (mutex-protected)
 *   Any thread: db_async_exec() - non-blocking enqueue
 */

#ifndef DB_MODULE_H
#define DB_MODULE_H

#include "mnvr_system.h"
#include <libpq-fe.h>

#define DB_WRITE_QUEUE_SIZE  256
#define DB_MAX_SQL_LEN       2048
#define DB_RECONNECT_SEC     5      /* seconds between reconnect attempts */

typedef struct {
    char sql[DB_MAX_SQL_LEN];
} DbWriteItem;

struct DbModule {
    AppContext      *ctx;

    /* libpq connection */
    PGconn         *conn;
    char            conninfo[MNVR_MAX_PATH];  /* "host=... dbname=..." */
    pthread_mutex_t conn_mutex;               /* serialises all PQexec calls */

    /* Async write queue */
    DbWriteItem      write_queue[DB_WRITE_QUEUE_SIZE];
    volatile int     q_head;
    volatile int     q_tail;
    pthread_mutex_t  q_mutex;
    pthread_cond_t   q_cond;
    pthread_t        writer_thread;
    volatile bool    running;

    char             schema_path[MNVR_MAX_PATH];
};

/* ---- Lifecycle ---- */
DbModule  *db_module_create(AppContext *ctx,
                              const char *conninfo,
                              const char *schema_path);
MnvrResult db_module_start(DbModule *dm);
void       db_module_stop(DbModule *dm);
void       db_module_destroy(DbModule *dm);

/* ---- Synchronous query (returns result via callback) ---- */
typedef void (*DbRowCallback)(void *user_data, int ncols,
                               char **vals, char **cols);
MnvrResult db_query(DbModule *dm, const char *sql,
                     DbRowCallback cb, void *user_data);

/* ---- Synchronous write (blocking, for startup/config) ---- */
MnvrResult db_exec(DbModule *dm, const char *sql);

/* ---- Async write (non-blocking, fire-and-forget for hot paths) ---- */
void db_async_exec(DbModule *dm, const char *sql);

/* ---- Schema application ---- */
MnvrResult db_apply_schema(DbModule *dm, const char *schema_path);

/* ---- Domain helpers ---- */
int64_t    db_insert_recording(DbModule *dm, int camera_id,
                                const char *file_path,
                                time_t start_time, int duration_sec,
                                int64_t file_size_bytes);

MnvrResult db_insert_event(DbModule *dm, int camera_id,
                             const char *event_type,
                             float confidence,
                             const char *metadata,
                             uint64_t pts_ms);

MnvrResult db_update_camera_state(DbModule *dm, int camera_id,
                                   const char *status);

/**
 * Fire-and-forget motion event insert (thin wrapper over db_insert_event).
 * Called from the AI worker thread's hot path — never blocks.
 */
MnvrResult db_insert_motion_event(DbModule *dm, int camera_id,
                                   float motion_ratio, uint64_t pts_ms);

/**
 * Insert a detected face row (fire-and-forget, async).
 * y_min/x_min/y_max/x_max are normalised [0,1]; frame_w/frame_h convert them
 * to pixel bbox columns. embedding may be NULL if no embedding was computed.
 * Returns -1 always (async path has no RETURNING id); do not depend on it.
 */
int db_insert_face_with_dedup_embed(DbModule *dm, int camera_id,
                                     float confidence,
                                     float y_min, float x_min,
                                     float y_max, float x_max,
                                     int frame_w, int frame_h,
                                     uint64_t pts_ms,
                                     const char *face_image_path,
                                     int dedup_window_minutes,
                                     int is_unique,
                                     const float *embedding,
                                     int embed_dim);

/* ---- HLS segment tracking ---- */

/**
 * Called by hls_module after successful MP4->TS conversion.
 * Updates the recordings row with HLS output details and inserts
 * a row into recording_segments for segment-level tracking.
 *
 * @param recording_id  The recordings.recording_id for the source MP4
 * @param ts_path       Full path to the produced .ts file
 * @param ts_size_bytes Size of the .ts file in bytes
 * @param duration_sec  Measured duration from GStreamer query
 * @param playlist_path Full path to the updated m3u8 playlist
 * @param segment_num   Segment sequence number (0-based)
 */
MnvrResult db_update_recording_hls(DbModule *dm,
                                    int64_t   recording_id,
                                    const char *ts_path,
                                    int64_t   ts_size_bytes,
                                    double    duration_sec,
                                    const char *playlist_path,
                                    int        segment_num);

/**
 * Mark a recording's HLS conversion as FAILED.
 * Called by hls_module when transcode_mp4_to_ts() returns -1.
 */
MnvrResult db_mark_recording_hls_failed(DbModule *dm, int64_t recording_id);

#endif /* DB_MODULE_H */
