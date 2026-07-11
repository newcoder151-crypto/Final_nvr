-- ============================================================================
-- MIGRATION: Add new settings tables from prev version
-- ============================================================================
-- Run this if you already have a v6 database and want to add the new
-- settings tables without re-importing the full schema.
--
-- Usage:
--   psql -h localhost -U mnvr -d mnvr -f migrate_v6_to_prev.sql
--
-- All statements are idempotent (IF NOT EXISTS / ON CONFLICT DO NOTHING).
-- ============================================================================

SET search_path = public;

-- ============================================================================
-- health_monitor_settings
-- ============================================================================
CREATE TABLE IF NOT EXISTS health_monitor_settings (
    id                      SERIAL PRIMARY KEY,
    poll_interval_sec       INTEGER  NOT NULL DEFAULT 10,
    cpu_warn_threshold      FLOAT    NOT NULL DEFAULT 85.0,
    mem_warn_threshold      FLOAT    NOT NULL DEFAULT 90.0,
    disk_warn_threshold     FLOAT    NOT NULL DEFAULT 90.0,
    cpu_critical_threshold  FLOAT    NOT NULL DEFAULT 95.0,
    mem_critical_threshold  FLOAT    NOT NULL DEFAULT 95.0,
    disk_critical_threshold FLOAT    NOT NULL DEFAULT 95.0,
    enable_alerts           BOOLEAN  NOT NULL DEFAULT TRUE,
    updated_at              TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_poll_interval     CHECK (poll_interval_sec > 0),
    CONSTRAINT chk_cpu_warn          CHECK (cpu_warn_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_mem_warn          CHECK (mem_warn_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_disk_warn         CHECK (disk_warn_threshold BETWEEN 0 AND 100)
);

INSERT INTO health_monitor_settings (poll_interval_sec)
    VALUES (10) ON CONFLICT DO NOTHING;

-- ============================================================================
-- recording_settings
-- ============================================================================
CREATE TABLE IF NOT EXISTS recording_settings (
    id                       SERIAL PRIMARY KEY,
    storage_path             TEXT     NOT NULL DEFAULT '/storage/recordings',
    recording_retention_days INTEGER  NOT NULL DEFAULT 30,
    segment_duration_sec     INTEGER  NOT NULL DEFAULT 120,
    segment_max_size_mb      INTEGER  NOT NULL DEFAULT 2048,
    max_storage_gb           INTEGER  NOT NULL DEFAULT 4000,
    enable_audio             BOOLEAN  NOT NULL DEFAULT TRUE,
    enable_watermark         BOOLEAN  NOT NULL DEFAULT TRUE,
    updated_at               TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_recording_retention CHECK (recording_retention_days > 0),
    CONSTRAINT chk_segment_duration    CHECK (segment_duration_sec > 0),
    CONSTRAINT chk_segment_size        CHECK (segment_max_size_mb > 0),
    CONSTRAINT chk_max_storage         CHECK (max_storage_gb > 0)
);

INSERT INTO recording_settings (storage_path)
    VALUES ('/storage/recordings') ON CONFLICT DO NOTHING;

-- ============================================================================
-- hls_settings
-- ============================================================================
CREATE TABLE IF NOT EXISTS hls_settings (
    id                      SERIAL PRIMARY KEY,
    hls_base                TEXT     NOT NULL DEFAULT '/storage/hls',
    hls_segment_sec         INTEGER  NOT NULL DEFAULT 4,
    hls_window_size         INTEGER  NOT NULL DEFAULT 10,
    hls_delete_old_segments BOOLEAN  NOT NULL DEFAULT TRUE,
    updated_at              TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_hls_segment CHECK (hls_segment_sec > 0),
    CONSTRAINT chk_hls_window  CHECK (hls_window_size > 0)
);

INSERT INTO hls_settings (hls_base)
    VALUES ('/storage/hls') ON CONFLICT DO NOTHING;

-- ============================================================================
-- onvif_settings
-- ============================================================================
CREATE TABLE IF NOT EXISTS onvif_settings (
    id                      SERIAL PRIMARY KEY,
    multicast_ip            VARCHAR(64)  NOT NULL DEFAULT '239.255.255.250',
    multicast_port          INTEGER      NOT NULL DEFAULT 3702,
    discovery_interval_sec  INTEGER      NOT NULL DEFAULT 60,
    probe_timeout_ms        INTEGER      NOT NULL DEFAULT 3000,
    enable_discovery        BOOLEAN      NOT NULL DEFAULT TRUE,
    updated_at              TIMESTAMP    NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_multicast_port      CHECK (multicast_port BETWEEN 1 AND 65535),
    CONSTRAINT chk_discovery_interval  CHECK (discovery_interval_sec > 0),
    CONSTRAINT chk_probe_timeout       CHECK (probe_timeout_ms > 0)
);

INSERT INTO onvif_settings (multicast_ip)
    VALUES ('239.255.255.250') ON CONFLICT DO NOTHING;

-- ============================================================================
-- cameras_config_details
-- ============================================================================
CREATE TABLE IF NOT EXISTS cameras_config_details (
    id               SERIAL PRIMARY KEY,
    camera_slot      INTEGER      UNIQUE NOT NULL,
    name             VARCHAR(128) NOT NULL,
    location         VARCHAR(128),
    camera_type      VARCHAR(32),
    ip_address       INET         NOT NULL UNIQUE,
    onvif_port       INTEGER      NOT NULL DEFAULT 80,
    onvif_username   VARCHAR(64),
    onvif_password   TEXT,
    rtsp_username    VARCHAR(64),
    rtsp_password    TEXT,
    manufacturer     VARCHAR(64),
    model            VARCHAR(64),
    is_active        BOOLEAN      NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP    NOT NULL DEFAULT NOW(),
    updated_at       TIMESTAMP    NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_onvif_port CHECK (onvif_port BETWEEN 1 AND 65535)
);

-- ============================================================================
-- cameras table: add new output-path columns if missing
-- ============================================================================
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS rec_output_dir TEXT;
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS hls_output_dir TEXT;
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS hls_playlist_url TEXT;

-- ============================================================================
-- face_detections: add embedding column if missing
-- ============================================================================
ALTER TABLE face_detections ADD COLUMN IF NOT EXISTS face_embedding BYTEA;

VACUUM ANALYZE;
