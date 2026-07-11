/**
 * @file db_module.c
 * @brief PostgreSQL database module - libpq implementation
 *
 * Uses libpq (PostgreSQL C client) instead of SQLite.
 *
 * Key differences from SQLite version:
 *   - conninfo string replaces file path: "host=localhost dbname=mnvr user=mnvr"
 *   - PQexec() replaces sqlite3_exec()
 *   - PQresultStatus() replaces SQLITE_OK checks
 *   - Reconnect logic handles dropped connections (network, server restart)
 *   - No WAL pragma - PostgreSQL handles durability internally
 *   - Schema applied via psql-compatible SQL (database_pg.sql)
 *   - db_insert_recording() uses PostgreSQL RETURNING clause for last insert id
 *   - datetime(X,'unixepoch') replaced with to_timestamp(X) in SQL strings
 *
 * Thread model:
 *   db_writer_thread: drains async ring-buffer queue
 *   conn_mutex: serialises ALL PQexec calls (libpq connection is not thread-safe)
 *
 * Connection string examples (set db_path in mnvr.conf):
 *   host=localhost port=5432 dbname=mnvr user=mnvr password=secret sslmode=disable
 *   host=/var/run/postgresql dbname=mnvr user=mnvr          (Unix socket)
 */

#define _POSIX_C_SOURCE 200809L

#include "db_module.h"
#include "../modules/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Internal: ensure connection is live, reconnect if needed
 * Caller MUST hold conn_mutex before calling this.
 * ------------------------------------------------------------------------- */
static bool ensure_connected(DbModule *dm)
{
    if (dm->conn && PQstatus(dm->conn) == CONNECTION_OK)
        return true;

    /* Close stale connection if any */
    if (dm->conn) {
        PQfinish(dm->conn);
        dm->conn = NULL;
    }

    LOG_INFO(dm->ctx, "DB", "Connecting to PostgreSQL: %s", dm->conninfo);
    dm->conn = PQconnectdb(dm->conninfo);

    if (!dm->conn || PQstatus(dm->conn) != CONNECTION_OK) {
        LOG_ERROR(dm->ctx, "DB", "Connection failed: %s",
                  dm->conn ? PQerrorMessage(dm->conn) : "out of memory");
        if (dm->conn) { PQfinish(dm->conn); dm->conn = NULL; }
        return false;
    }

    LOG_INFO(dm->ctx, "DB", "PostgreSQL connected (server v%d)",
             PQserverVersion(dm->conn));
    return true;
}

/* -------------------------------------------------------------------------
 * Internal: execute SQL and check result, log errors
 * Caller MUST hold conn_mutex.
 * ------------------------------------------------------------------------- */
static bool pg_exec_internal(DbModule *dm, const char *sql)
{
    if (!ensure_connected(dm)) return false;

    PGresult *res = PQexec(dm->conn, sql);
    ExecStatusType status = PQresultStatus(res);

    bool ok = (status == PGRES_COMMAND_OK ||
               status == PGRES_TUPLES_OK  ||
               status == PGRES_EMPTY_QUERY);

    if (!ok) {
        LOG_ERROR(dm->ctx, "DB", "Exec error [%s]: %s | SQL: %.120s",
                  PQresStatus(status),
                  PQresultErrorMessage(res),
                  sql);
    }

    PQclear(res);
    return ok;
}

/* -------------------------------------------------------------------------
 * Async writer thread
 * Drains the ring-buffer queue, executing each SQL statement.
 * Retries with exponential back-off on connection failure.
 * ------------------------------------------------------------------------- */
static void *db_writer_thread_fn(void *arg)
{
    DbModule *dm = (DbModule *)arg;
    LOG_INFO(dm->ctx, "DB", "Async writer thread started");

    while (dm->running || dm->q_tail != dm->q_head) {
        pthread_mutex_lock(&dm->q_mutex);
        while (dm->q_tail == dm->q_head && dm->running)
            pthread_cond_wait(&dm->q_cond, &dm->q_mutex);

        while (dm->q_tail != dm->q_head) {
            DbWriteItem item = dm->write_queue[dm->q_tail % DB_WRITE_QUEUE_SIZE];
            dm->q_tail++;
            pthread_mutex_unlock(&dm->q_mutex);

            /* Execute under connection mutex */
            pthread_mutex_lock(&dm->conn_mutex);
            pg_exec_internal(dm, item.sql);
            pthread_mutex_unlock(&dm->conn_mutex);

            pthread_mutex_lock(&dm->q_mutex);
        }
        pthread_mutex_unlock(&dm->q_mutex);
    }

    LOG_INFO(dm->ctx, "DB", "Async writer thread stopped");
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

DbModule *db_module_create(AppContext *ctx,
                             const char *conninfo,
                             const char *schema_path)
{
    DbModule *dm = calloc(1, sizeof(DbModule));
    if (!dm) return NULL;
    dm->ctx = ctx;
    strncpy(dm->conninfo,     conninfo,     MNVR_MAX_PATH - 1);
    strncpy(dm->schema_path,  schema_path,  MNVR_MAX_PATH - 1);

    pthread_mutex_init(&dm->conn_mutex, NULL);
    pthread_mutex_init(&dm->q_mutex,    NULL);
    pthread_cond_init(&dm->q_cond,      NULL);
    return dm;
}

MnvrResult db_module_start(DbModule *dm)
{
    if (!dm) return MNVR_ERR_GENERIC;

    /* Initial connection */
    pthread_mutex_lock(&dm->conn_mutex);
    bool ok = ensure_connected(dm);
    pthread_mutex_unlock(&dm->conn_mutex);

    if (!ok) {
        LOG_FATAL(dm->ctx, "DB",
                  "Cannot connect to PostgreSQL. Check conninfo: %s",
                  dm->conninfo);
        return MNVR_ERR_DB;
    }

    /* Apply schema on first run */
    if (dm->schema_path[0]) {
        MnvrResult r = db_apply_schema(dm, dm->schema_path);
        if (r != MNVR_OK)
            LOG_WARN(dm->ctx, "DB",
                     "Schema apply returned errors (may be OK if already applied)");
    }

    /* Start async writer thread */
    dm->running = true;
    if (pthread_create(&dm->writer_thread, NULL, db_writer_thread_fn, dm) != 0) {
        LOG_FATAL(dm->ctx, "DB", "Failed to start writer thread");
        PQfinish(dm->conn);
        dm->conn = NULL;
        return MNVR_ERR_GENERIC;
    }

    LOG_INFO(dm->ctx, "DB", "PostgreSQL module started");
    return MNVR_OK;
}

void db_module_stop(DbModule *dm)
{
    if (!dm) return;
    pthread_mutex_lock(&dm->q_mutex);
    dm->running = false;
    pthread_cond_signal(&dm->q_cond);
    pthread_mutex_unlock(&dm->q_mutex);
    pthread_join(dm->writer_thread, NULL);
    LOG_INFO(dm->ctx, "DB", "Writer thread stopped");
}

void db_module_destroy(DbModule *dm)
{
    if (!dm) return;
    db_module_stop(dm);
    pthread_mutex_lock(&dm->conn_mutex);
    if (dm->conn) {
        PQfinish(dm->conn);
        dm->conn = NULL;
    }
    pthread_mutex_unlock(&dm->conn_mutex);
    pthread_mutex_destroy(&dm->conn_mutex);
    pthread_mutex_destroy(&dm->q_mutex);
    pthread_cond_destroy(&dm->q_cond);
    free(dm);
}

MnvrResult db_query(DbModule *dm, const char *sql,
                     DbRowCallback cb, void *user_data)
{
    if (!dm) return MNVR_ERR_DB;

    pthread_mutex_lock(&dm->conn_mutex);
    if (!ensure_connected(dm)) {
        pthread_mutex_unlock(&dm->conn_mutex);
        return MNVR_ERR_DB;
    }

    PGresult *res = PQexec(dm->conn, sql);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        LOG_ERROR(dm->ctx, "DB", "Query error [%s]: %s",
                  PQresStatus(status), PQresultErrorMessage(res));
        PQclear(res);
        pthread_mutex_unlock(&dm->conn_mutex);
        return MNVR_ERR_DB;
    }

    /* Invoke callback for each row */
    if (cb && status == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        int ncols = PQnfields(res);

        /* Build column name array */
        char **cols = malloc(ncols * sizeof(char *));
        char **vals = malloc(ncols * sizeof(char *));
        if (cols && vals) {
            for (int col = 0; col < ncols; col++)
                cols[col] = PQfname(res, col);

            for (int row = 0; row < nrows; row++) {
                for (int col = 0; col < ncols; col++)
                    vals[col] = PQgetisnull(res, row, col)
                                ? NULL
                                : PQgetvalue(res, row, col);
                cb(user_data, ncols, vals, cols);
            }
        }
        free(cols);
        free(vals);
    }

    PQclear(res);
    pthread_mutex_unlock(&dm->conn_mutex);
    return MNVR_OK;
}

MnvrResult db_exec(DbModule *dm, const char *sql)
{
    if (!dm) return MNVR_ERR_DB;
    pthread_mutex_lock(&dm->conn_mutex);
    bool ok = pg_exec_internal(dm, sql);
    pthread_mutex_unlock(&dm->conn_mutex);
    return ok ? MNVR_OK : MNVR_ERR_DB;
}

void db_async_exec(DbModule *dm, const char *sql)
{
    if (!dm) return;
    pthread_mutex_lock(&dm->q_mutex);
    if ((dm->q_head - dm->q_tail) >= DB_WRITE_QUEUE_SIZE) {
        /* Ring full - drop oldest entry, warn once */
        LOG_WARN(dm->ctx, "DB", "Async write queue full - dropping oldest entry");
        dm->q_tail++;   /* overwrite oldest */
    }
    DbWriteItem *item = &dm->write_queue[dm->q_head % DB_WRITE_QUEUE_SIZE];
    strncpy(item->sql, sql, DB_MAX_SQL_LEN - 1);
    item->sql[DB_MAX_SQL_LEN - 1] = '\0';
    dm->q_head++;
    pthread_cond_signal(&dm->q_cond);
    pthread_mutex_unlock(&dm->q_mutex);
}

MnvrResult db_apply_schema(DbModule *dm, const char *schema_path)
{
    FILE *fp = fopen(schema_path, "r");
    if (!fp) {
        LOG_WARN(dm->ctx, "DB", "Schema file not found: %s", schema_path);
        return MNVR_ERR_IO;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return MNVR_ERR_NOMEM; }

    if (fread(buf, 1, sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return MNVR_ERR_IO;
    }
    buf[sz] = '\0';
    fclose(fp);

    /*
     * The schema file already contains its own BEGIN...COMMIT block.
     * We execute it directly without an outer transaction wrapper so that:
     *  1. No nested-transaction warnings from PostgreSQL
     *  2. VACUUM ANALYZE (which cannot run inside a transaction) works correctly
     *
     * PostgreSQL simple-query protocol (PQexec) supports multiple semicolon-
     * separated statements in a single call, so the entire file is sent at once.
     *
     * VACUUM is stripped from the buffer and run separately after the main
     * schema SQL completes, because VACUUM cannot run inside a transaction block.
     */
    pthread_mutex_lock(&dm->conn_mutex);

    bool ok = true;

    /* Separate VACUUM ANALYZE from the rest - it cannot run inside a transaction */
    char *vacuum_pos = strstr(buf, "VACUUM");
    if (vacuum_pos) *vacuum_pos = '\0';   /* terminate buf before VACUUM */

    /* Execute schema (includes BEGIN...COMMIT internally) */
    PGresult *res = PQexec(dm->conn, buf);
    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        LOG_WARN(dm->ctx, "DB", "Schema apply warning [%s]: %s",
                 PQresStatus(status), PQresultErrorMessage(res));
        PQclear(res);
        ok = false;
    } else {
        PQclear(res);
        LOG_INFO(dm->ctx, "DB", "Schema applied from %s", schema_path);

        /* Run VACUUM ANALYZE outside transaction */
        res = PQexec(dm->conn, "VACUUM ANALYZE;");
        PQclear(res);
    }

    free(buf);
    pthread_mutex_unlock(&dm->conn_mutex);
    return ok ? MNVR_OK : MNVR_ERR_DB;
}

int64_t db_insert_recording(DbModule *dm, int camera_id,
                              const char *file_path,
                              time_t start_time, int duration_sec,
                              int64_t file_size_bytes)
{
    char sql[DB_MAX_SQL_LEN];
    /* Extract filename from path for file_name (NOT NULL) column */
    const char *slash = strrchr(file_path, '/');
    const char *file_name = slash ? slash + 1 : file_path;

    /* Use correct column names from database_pg.sql schema */
    snprintf(sql, sizeof(sql),
        "INSERT INTO recordings "
        "(camera_id, file_path, file_name, start_timestamp, end_timestamp, "
        " duration_seconds, file_size_bytes, status, created_at) "
        "VALUES (%d, '%s', '%s', to_timestamp(%ld), "
        "to_timestamp(%ld), %d, %lld, 'COMPLETED', NOW()) "
        "RETURNING recording_id;",
        camera_id, file_path, file_name,
        (long)start_time, (long)(start_time + duration_sec),
        duration_sec, (long long)file_size_bytes);

    /* Execute synchronously to get RETURNING value */
    int64_t new_id = -1;
    pthread_mutex_lock(&dm->conn_mutex);
    if (ensure_connected(dm)) {
        PGresult *res = PQexec(dm->conn, sql);
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
            new_id = atoll(PQgetvalue(res, 0, 0));
        else
            LOG_ERROR(dm->ctx, "DB", "insert_recording failed: %s",
                      PQresultErrorMessage(res));
        PQclear(res);
    }
    pthread_mutex_unlock(&dm->conn_mutex);
    return new_id;
}

MnvrResult db_insert_event(DbModule *dm, int camera_id,
                             const char *event_type,
                             float confidence,
                             const char *metadata,
                             uint64_t pts_ms)
{
    char sql[DB_MAX_SQL_LEN];
    /* Build a human-readable title from event_type */
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s detected on camera %d",
             event_type, camera_id);

    /* Use correct columns from database_pg.sql schema.
     * confidence and pts_ms stored in event_data JSON. */
    char event_json[DB_MAX_SQL_LEN / 2];
    snprintf(event_json, sizeof(event_json),
             "{\"confidence\":%.4f,\"pts_ms\":%llu,\"detail\":%s}",
             confidence, (unsigned long long)pts_ms,
             metadata ? metadata : "{}");

    snprintf(sql, sizeof(sql),
        "INSERT INTO events "
        "(camera_id, event_type, title, event_data, "
        " source_device_type, occurred_at, created_at) "
        "VALUES (%d, '%s', '%s', '%s', 'CAMERA', NOW(), NOW());",
        camera_id, event_type, title_buf, event_json);

    db_async_exec(dm, sql);
    return MNVR_OK;
}

MnvrResult db_insert_motion_event(DbModule *dm, int camera_id,
                                   float motion_ratio, uint64_t pts_ms)
{
    char meta[128];
    snprintf(meta, sizeof(meta), "{\"motion_ratio\":%.4f}", motion_ratio);
    return db_insert_event(dm, camera_id, "MOTION", motion_ratio, meta, pts_ms);
}

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
                                     int embed_dim)
{
    if (!dm) return -1;

    int bbox_x = (int)(x_min * frame_w);
    int bbox_y = (int)(y_min * frame_h);
    int bbox_w = (int)((x_max - x_min) * frame_w);
    int bbox_h = (int)((y_max - y_min) * frame_h);
    if (bbox_x < 0) bbox_x = 0;
    if (bbox_y < 0) bbox_y = 0;
    if (bbox_w < 0) bbox_w = 0;
    if (bbox_h < 0) bbox_h = 0;

    /* Encode embedding bytes as a Postgres hex-bytea literal E'\\xDEADBEEF'
     * so it can travel through the plain-text async write queue safely
     * (no NUL-byte / binary-in-SQL-string issues). Sized dynamically since
     * EMBED_DIM (512 floats -> 2048 bytes -> 4096 hex chars) does not fit
     * in the fixed DB_MAX_SQL_LEN write-queue slot alongside it, so this
     * insert bypasses the ring buffer and executes on the calling thread. */
    size_t nbytes = (embedding && embed_dim > 0) ? (size_t)embed_dim * sizeof(float) : 0;
    size_t literal_cap = nbytes ? (nbytes * 2 + 8) : 8;
    char *embed_literal = (char *)malloc(literal_cap);
    if (!embed_literal) return -1;
    if (nbytes) {
        static const char hexch[] = "0123456789abcdef";
        char *p = embed_literal;
        *p++ = 'E'; *p++ = '\''; *p++ = '\\'; *p++ = 'x';
        const unsigned char *bytes = (const unsigned char *)embedding;
        for (size_t i = 0; i < nbytes; i++) {
            *p++ = hexch[(bytes[i] >> 4) & 0xF];
            *p++ = hexch[bytes[i] & 0xF];
        }
        *p++ = '\''; *p = '\0';
    } else {
        strcpy(embed_literal, "NULL");
    }

    size_t sql_cap = literal_cap + 512;
    char *sql = (char *)malloc(sql_cap);
    if (!sql) { free(embed_literal); return -1; }

    snprintf(sql, sql_cap,
        "INSERT INTO face_detections "
        "(camera_id, detection_timestamp, face_image_path, "
        " bbox_x, bbox_y, bbox_width, bbox_height, quality_score, "
        " is_unique, face_embedding, created_at) "
        "VALUES (%d, to_timestamp(%.3f), '%s', "
        "%d, %d, %d, %d, %.4f, %d, %s, NOW());",
        camera_id, (double)pts_ms / 1000.0, face_image_path,
        bbox_x, bbox_y, bbox_w, bbox_h, confidence,
        is_unique ? 1 : 0, embed_literal);

    (void)dedup_window_minutes; /* informational only, not persisted as a column */

    /* Execute directly (not db_async_exec) because the embedding hex string
     * can exceed DB_MAX_SQL_LEN, the fixed size of each write-queue slot. */
    pthread_mutex_lock(&dm->conn_mutex);
    if (ensure_connected(dm)) {
        PGresult *res = PQexec(dm->conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            LOG_ERROR(dm->ctx, "DB", "insert_face failed: %s",
                      PQresultErrorMessage(res));
        PQclear(res);
    }
    pthread_mutex_unlock(&dm->conn_mutex);

    free(sql);
    free(embed_literal);
    return -1; /* fire-and-forget: no synchronous id available */
}

MnvrResult db_update_camera_state(DbModule *dm, int camera_id,
                                   const char *status)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE cameras SET status='%s', last_seen_at=NOW() "
        "WHERE camera_id=%d;",
        status, camera_id);
    db_async_exec(dm, sql);
    return MNVR_OK;
}

MnvrResult db_update_recording_hls(DbModule *dm,
                                    int64_t   recording_id,
                                    const char *ts_path,
                                    int64_t   ts_size_bytes,
                                    double    duration_sec,
                                    const char *playlist_path,
                                    int        segment_num)
{
    if (!dm || recording_id < 0) return MNVR_ERR_GENERIC;

    /* 1. Update the recordings row with HLS output details */
    char sql[DB_MAX_SQL_LEN];
    snprintf(sql, sizeof(sql),
        "UPDATE recordings "
        "SET hls_ts_file_path        = '%s', "
        "    hls_ts_file_size_bytes  = %lld, "
        "    hls_segment_duration_sec = %.3f, "
        "    hls_playlist_path       = '%s', "
        "    hls_conversion_status   = 'COMPLETED', "
        "    hls_converted_at        = NOW(), "
        "    updated_at              = NOW() "
        "WHERE recording_id = %lld;",
        ts_path,
        (long long)ts_size_bytes,
        duration_sec,
        playlist_path ? playlist_path : "",
        (long long)recording_id);
    db_async_exec(dm, sql);

    /* 2. Insert a recording_segments row for segment-level tracking */
    char seg_sql[DB_MAX_SQL_LEN];
    snprintf(seg_sql, sizeof(seg_sql),
        "INSERT INTO recording_segments "
        "(recording_id, segment_number, segment_file_path, "
        " segment_start_timestamp, "
        " hls_ts_file_path, hls_ts_size_bytes, hls_ts_duration_sec, "
        " hls_conversion_status, hls_converted_at) "
        "VALUES (%lld, %d, "
        "  (SELECT file_path FROM recordings WHERE recording_id=%lld), "
        "  (SELECT start_timestamp FROM recordings WHERE recording_id=%lld), "
        "  '%s', %lld, %.3f, 'COMPLETED', NOW()) "
        "ON CONFLICT (recording_id, segment_number) DO UPDATE "
        "SET hls_ts_file_path=EXCLUDED.hls_ts_file_path, "
        "    hls_ts_size_bytes=EXCLUDED.hls_ts_size_bytes, "
        "    hls_ts_duration_sec=EXCLUDED.hls_ts_duration_sec, "
        "    hls_conversion_status='COMPLETED', "
        "    hls_converted_at=NOW();",
        (long long)recording_id, segment_num,
        (long long)recording_id,
        (long long)recording_id,
        ts_path, (long long)ts_size_bytes, duration_sec);
    db_async_exec(dm, seg_sql);

    return MNVR_OK;
}

MnvrResult db_mark_recording_hls_failed(DbModule *dm, int64_t recording_id)
{
    if (!dm || recording_id < 0) return MNVR_ERR_GENERIC;
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE recordings "
        "SET hls_conversion_status='FAILED', updated_at=NOW() "
        "WHERE recording_id=%lld;",
        (long long)recording_id);
    db_async_exec(dm, sql);
    return MNVR_OK;
}
