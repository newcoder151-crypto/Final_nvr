-- ============================================================================
-- mNVR SYSTEM - PostgreSQL Database Schema
-- ============================================================================
-- Version  : 1.0.0
-- Database : PostgreSQL 14+
-- Encoding : UTF-8
--
-- IMPORT INSTRUCTIONS
-- -------------------
-- 1. Create database and user (run as postgres superuser):
--
--    CREATE USER mnvr WITH PASSWORD 'your_password_here';
--    CREATE DATABASE mnvr OWNER mnvr ENCODING 'UTF8';
--    GRANT ALL PRIVILEGES ON DATABASE mnvr TO mnvr;
--
-- 2. Import this file:
--
--    psql -h localhost -U mnvr -d mnvr -f mnvr_schema.sql
--
--    Or with password prompt suppressed (use .pgpass file):
--    PGPASSWORD=your_password psql -h localhost -U mnvr -d mnvr -f mnvr_schema.sql
--
-- 3. Verify import:
--
--    psql -h localhost -U mnvr -d mnvr -c "\dt"   -- list all tables
--    psql -h localhost -U mnvr -d mnvr -c "SELECT COUNT(*) FROM cameras;"
--
-- IDEMPOTENCY
-- -----------
-- All CREATE TABLE / CREATE INDEX statements use IF NOT EXISTS.
-- All INSERT statements use ON CONFLICT DO NOTHING.
-- This file is safe to run multiple times on the same database.
--
-- CAMERAS TABLE - NEW COLUMNS (v1.0.0)
-- -------------------------------------
-- rec_output_dir    TEXT  - MP4 recording output directory per camera
--                           e.g.  /storage/recordings/cam_1
--                           Populated by the mNVR application at startup
--                           based on storage_base config + camera_id
--
-- hls_output_dir    TEXT  - HLS .ts segment output directory per camera
--                           e.g.  /storage/hls/cam_1
--                           Populated by the mNVR application at startup
--                           based on hls_base config + camera_id
--
-- hls_playlist_url  TEXT  - Public HLS playlist URL served by the API
--                           e.g.  /hls/cam_1/stream.m3u8
--                           Set at startup; used by web app for live view
--
-- RECOMMENDED postgresql.conf SETTINGS (for embedded NVR use)
-- ------------------------------------------------------------
-- shared_buffers            = 256MB
-- work_mem                  = 4MB
-- synchronous_commit        = off      -- safe for NVR; minor loss on crash OK
-- wal_level                 = minimal
-- max_wal_size              = 1GB
-- checkpoint_completion_target = 0.9
-- log_min_duration_statement = 1000    -- log slow queries > 1s
-- ============================================================================


-- Set client encoding
SET client_encoding = 'UTF8';

-- Use public schema
SET search_path = public;

BEGIN;

-- ============================================================================
-- mNVR SYSTEM - COMPLETE DATABASE SCHEMA
-- ============================================================================
-- Document Version: 1.0
-- Date: October 13, 2025
-- Database: PostgreSQL 14+
-- Purpose: Complete database schema for Mobile Network Video Recorder System
-- 
-- This schema supports:
-- - Video/Audio Recording Management
-- - GPS/Time Synchronization (PTP/NTP/GPS)
-- - Camera-Microphone Mapping
-- - Event and Alarm Management
-- - User Management and Authentication
-- - System Health Monitoring
-- - Face Recognition Data
-- - RDAS Integration
-- - CHM (Crash Hardened Memory) Operations
-- - Audit Logging
-- ============================================================================

-- ============================================================================
-- SECTION 1: SYSTEM CONFIGURATION
-- ============================================================================

-- Device identification and system configuration
CREATE TABLE IF NOT EXISTS system_config (
    id SERIAL PRIMARY KEY,
    config_key TEXT NOT NULL UNIQUE,
    config_value TEXT NOT NULL,
    config_type TEXT NOT NULL DEFAULT 'STRING', -- STRING, INTEGER, BOOLEAN, JSON
    description TEXT,
    is_readonly INTEGER DEFAULT 0, -- 1 for system-managed, 0 for user-configurable
    last_modified_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_modified_by TEXT,
    CONSTRAINT chk_config_type CHECK (config_type IN ('STRING', 'INTEGER', 'BOOLEAN', 'JSON'))
);

-- Default system configuration entries
INSERT INTO system_config (config_key, config_value, config_type, description, is_readonly) VALUES
('device_id', '', 'STRING', 'Unique MNVR device identifier (serial number)', 0),
('device_name', 'MNVR-001', 'STRING', 'Human-readable device name', 0),
('firmware_version', '1.0.0', 'STRING', 'Current firmware version', 1),
('installation_date', '', 'STRING', 'Date of system installation', 0),
('coach_number', '', 'STRING', 'Train coach number', 0),
('train_number', '', 'STRING', 'Train identification number', 0),
('recording_retention_days', '30', 'INTEGER', 'Number of days to retain recordings', 0),
('storage_path', '/storage/recordings', 'STRING', 'Base path for video storage', 1),
('max_storage_gb', '4000', 'INTEGER', 'Maximum storage capacity in GB', 0),
('enable_audio', '1', 'BOOLEAN', 'Enable audio recording', 0),
('enable_gps', '1', 'BOOLEAN', 'Enable GPS tracking', 0),
('enable_watermark', '1', 'BOOLEAN', 'Enable video watermarking', 0),
('enable_face_detection', '1', 'BOOLEAN', 'Enable face detection/cropping', 0),
('time_sync_method', 'PTP', 'STRING', 'Primary time sync: PTP, NTP, GPS', 0),
('ptp_domain', '0', 'INTEGER', 'PTP domain number', 0),
('ntp_server', 'pool.ntp.org', 'STRING', 'NTP server address', 0),
('api_server_port', '8443', 'INTEGER', 'HTTPS API server port', 0),
('rtsp_server_port', '554', 'INTEGER', 'RTSP streaming port', 0)
ON CONFLICT (config_key) DO NOTHING;

-- ============================================================================
-- SECTION 2: CAMERA MANAGEMENT
-- ============================================================================

-- Camera configuration and status
CREATE TABLE IF NOT EXISTS cameras (
    camera_id SERIAL PRIMARY KEY,
    camera_name TEXT NOT NULL,
    camera_type TEXT NOT NULL, -- INTERIOR, EXTERIOR, DOOR, DRIVER_CAB
    onvif_device_id TEXT UNIQUE,
    ip_address TEXT NOT NULL,
    rtsp_url TEXT NOT NULL,
    -- Dedicated recording-profile RTSP URL, when the camera exposes a
    -- separate profile for it (see onvif_module.c dual-profile selection).
    -- NULL/empty means "same as rtsp_url" — recorder.c falls back to
    -- rtsp_url in that case.
    rec_rtsp_url TEXT,
    rtsp_port INTEGER DEFAULT 554,
    username TEXT,
    password_hash TEXT,
    manufacturer TEXT,
    model TEXT,
    firmware_version TEXT,
    resolution_width INTEGER DEFAULT 1920,
    resolution_height INTEGER DEFAULT 1080,
    target_fps INTEGER DEFAULT 25, -- 25 for interior, 45 for exterior
    video_codec TEXT DEFAULT 'H.265',
    location_description TEXT, -- e.g., "Front entrance door", "Passenger area rear"
    physical_position TEXT, -- GPS coordinates or coach position
    ptz_supported INTEGER DEFAULT 0,
    audio_supported INTEGER DEFAULT 0,
    status TEXT DEFAULT 'ACTIVE', -- ACTIVE, INACTIVE, FAULTY, MAINTENANCE
    is_ptp_synced INTEGER DEFAULT 0,
    is_ntp_synced INTEGER DEFAULT 0,
    last_seen_at TIMESTAMP,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    -- Output paths (set by application at provisioning time)
    rec_output_dir TEXT,                         -- MP4 recording output directory, e.g. /storage/recordings/cam_1
    hls_output_dir TEXT,                         -- HLS segments output directory,  e.g. /storage/hls/cam_1
    hls_playlist_url TEXT,                       -- Public HLS playlist URL,         e.g. /hls/cam_1/stream.m3u8
    CONSTRAINT chk_camera_type CHECK (camera_type IN ('INTERIOR', 'EXTERIOR', 'DOOR', 'DRIVER_CAB')),
    CONSTRAINT chk_camera_status CHECK (status IN ('ACTIVE', 'INACTIVE', 'FAULTY', 'MAINTENANCE')),
    CONSTRAINT chk_video_codec CHECK (video_codec IN ('H.264', 'H.265', 'H.265+'))
);

-- ONVIF device discovery results
CREATE TABLE IF NOT EXISTS device_discovery (
    discovery_id SERIAL PRIMARY KEY,
    device_uuid TEXT,
    ip_address TEXT NOT NULL UNIQUE,
    manufacturer TEXT,
    model TEXT,
    firmware_version TEXT,
    serial_number TEXT,
    xaddrs TEXT,
    stream_uri TEXT,
    snapshot_uri TEXT,
    ptz_supported BOOLEAN DEFAULT false,
    audio_supported BOOLEAN DEFAULT false,
    imaging_supported BOOLEAN DEFAULT false,
    events_supported BOOLEAN DEFAULT false,
    media_service_url TEXT,
    ptz_service_url TEXT,
    imaging_service_url TEXT,
    events_service_url TEXT,
    discovered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_probed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    probe_status TEXT DEFAULT 'OK'
);

CREATE INDEX IF NOT EXISTS idx_device_discovery_ip ON device_discovery(ip_address);

-- Camera health metrics
CREATE TABLE IF NOT EXISTS camera_health (
    health_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    is_online INTEGER DEFAULT 1,
    is_recording INTEGER DEFAULT 1,
    frame_rate_actual DOUBLE PRECISION,
    bitrate_kbps INTEGER,
    packet_loss_percent DOUBLE PRECISION DEFAULT 0.0,
    latency_ms INTEGER,
    temperature_celsius DOUBLE PRECISION,
    uptime_seconds INTEGER,
    error_count INTEGER DEFAULT 0,
    last_error TEXT,
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE
);

-- Camera tampering events
CREATE TABLE IF NOT EXISTS camera_tampering_events (
    event_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    event_type TEXT NOT NULL, -- OBSTRUCTION, REDIRECTION, BLINDING, FOCUS_LOSS
    severity TEXT DEFAULT 'MEDIUM', -- LOW, MEDIUM, HIGH, CRITICAL
    detected_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    resolved_at TIMESTAMP,
    status TEXT DEFAULT 'ACTIVE', -- ACTIVE, ACKNOWLEDGED, RESOLVED
    confidence_score DOUBLE PRECISION, -- 0.0 to 1.0
    snapshot_path TEXT,
    description TEXT,
    acknowledged_by TEXT,
    acknowledged_at TIMESTAMP,
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    CONSTRAINT chk_tampering_type CHECK (event_type IN ('OBSTRUCTION', 'REDIRECTION', 'BLINDING', 'FOCUS_LOSS')),
    CONSTRAINT chk_tampering_severity CHECK (severity IN ('LOW', 'MEDIUM', 'HIGH', 'CRITICAL')),
    CONSTRAINT chk_tampering_status CHECK (status IN ('ACTIVE', 'ACKNOWLEDGED', 'RESOLVED'))
);

-- ============================================================================
-- SECTION 3: AUDIO MANAGEMENT
-- ============================================================================

-- Microphone configuration
CREATE TABLE IF NOT EXISTS microphones (
    mic_id SERIAL PRIMARY KEY,
    mic_name TEXT NOT NULL,
    ip_address TEXT,
    rtsp_url TEXT,
    audio_codec TEXT DEFAULT 'AAC', -- AAC, G.711, G.722, G.726
    sample_rate INTEGER DEFAULT 48000,
    channels INTEGER DEFAULT 2, -- 1=mono, 2=stereo
    bitrate_kbps INTEGER DEFAULT 128,
    location_description TEXT,
    status TEXT DEFAULT 'ACTIVE',
    is_ptp_synced INTEGER DEFAULT 0,
    is_ntp_synced INTEGER DEFAULT 0,
    last_seen_at TIMESTAMP,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT chk_audio_codec CHECK (audio_codec IN ('AAC', 'G.711', 'G.722', 'G.726')),
    CONSTRAINT chk_mic_status CHECK (status IN ('ACTIVE', 'INACTIVE', 'FAULTY', 'MAINTENANCE'))
);

-- Camera-Microphone mapping for audio synchronization
CREATE TABLE IF NOT EXISTS camera_mic_mapping (
    mapping_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    mic_id INTEGER NOT NULL,
    latency_offset_ms INTEGER DEFAULT 0, -- Audio delay correction in milliseconds
    is_active INTEGER DEFAULT 1,
    calibration_date TIMESTAMP,
    calibration_method TEXT, -- MANUAL, AUTO_CLAPPER, CROSS_CORRELATION
    calibration_confidence DOUBLE PRECISION,
    notes TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    FOREIGN KEY (mic_id) REFERENCES microphones(mic_id) ON DELETE CASCADE,
    UNIQUE(camera_id, mic_id)
);

-- ============================================================================
-- SECTION 4: RECORDINGS MANAGEMENT
-- ============================================================================

-- Master recordings table
CREATE TABLE IF NOT EXISTS recordings (
    recording_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    audio_stream_id INTEGER, -- References mic_id from microphones table
    file_path TEXT NOT NULL UNIQUE,
    file_name TEXT NOT NULL,
    file_size_bytes INTEGER,
    duration_seconds INTEGER,
    start_timestamp TIMESTAMP NOT NULL,
    end_timestamp TIMESTAMP,
    
    -- Video properties
    video_codec TEXT DEFAULT 'H.265',
    resolution_width INTEGER DEFAULT 1920,
    resolution_height INTEGER DEFAULT 1080,
    fps_actual DOUBLE PRECISION,
    video_bitrate_kbps INTEGER,
    
    -- Audio properties
    has_audio INTEGER DEFAULT 0,
    audio_codec TEXT,
    audio_sample_rate INTEGER,
    audio_channels INTEGER,
    audio_bitrate_kbps INTEGER,
    
    -- Synchronization metadata
    time_source TEXT DEFAULT 'PTP', -- PTP, NTP, GPS, SYSTEM
    ptp_offset_ns INTEGER, -- PTP clock offset in nanoseconds
    ntp_offset_ms INTEGER, -- NTP clock offset in milliseconds
    sync_quality_ms DOUBLE PRECISION, -- Overall sync quality estimate
    audio_sync_offset_ms INTEGER, -- Applied audio latency correction
    
    -- GPS metadata
    gps_latitude DOUBLE PRECISION,
    gps_longitude DOUBLE PRECISION,
    gps_altitude DOUBLE PRECISION,
    gps_speed_kmh DOUBLE PRECISION,
    gps_heading DOUBLE PRECISION,
    gps_satellites INTEGER,
    gps_fix_quality TEXT, -- NO_FIX, 2D, 3D, DGPS
    
    -- Security and integrity
    watermark_enabled INTEGER DEFAULT 0,
    watermark_hash TEXT, -- SHA-256 hash of embedded watermark
    file_integrity_hash TEXT, -- SHA-256 hash of entire file
    is_tampered INTEGER DEFAULT 0,
    
    -- Recording metadata
    recording_mode TEXT DEFAULT 'CONTINUOUS', -- CONTINUOUS, MOTION, ALARM, MANUAL
    trigger_event_id INTEGER, -- References events table if alarm-triggered
    segment_number INTEGER, -- For splitmuxsink 10-minute segments
    
    -- HLS live-view output (populated by HLS module after MP4->TS conversion)
    hls_ts_file_path TEXT,           -- MPEG-TS file produced from this segment, e.g. /storage/hls/cam_1/seg_00001.ts
    hls_ts_file_size_bytes INTEGER,  -- Size of the .ts file in bytes
    hls_segment_duration_sec DOUBLE PRECISION, -- Actual .ts segment duration measured by GStreamer
    hls_playlist_path TEXT,          -- Path to the m3u8 playlist updated after this segment
    hls_conversion_status TEXT DEFAULT 'PENDING', -- PENDING, COMPLETED, FAILED
    hls_converted_at TIMESTAMP,      -- When the .ts was produced

    -- Status
    status TEXT DEFAULT 'RECORDING', -- RECORDING, COMPLETED, EXPORTED, ARCHIVED, DELETED
    is_uploaded_to_cloud INTEGER DEFAULT 0,
    cloud_upload_timestamp TIMESTAMP,
    is_backed_up_to_chm INTEGER DEFAULT 0,
    chm_backup_timestamp TIMESTAMP,
    
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    FOREIGN KEY (audio_stream_id) REFERENCES microphones(mic_id) ON DELETE SET NULL,
    -- trigger_event_id FK to events added below via ALTER TABLE (events table defined later)
    
    CONSTRAINT chk_recording_status CHECK (status IN ('RECORDING', 'COMPLETED', 'EXPORTED', 'ARCHIVED', 'DELETED')),
    CONSTRAINT chk_recording_mode CHECK (recording_mode IN ('CONTINUOUS', 'MOTION', 'ALARM', 'MANUAL')),
    CONSTRAINT chk_gps_fix CHECK (gps_fix_quality IN ('NO_FIX', '2D', '3D', 'DGPS', NULL)),
    CONSTRAINT chk_hls_status CHECK (hls_conversion_status IN ('PENDING', 'COMPLETED', 'FAILED'))
);

-- Index for performance optimization
CREATE INDEX IF NOT EXISTS idx_recordings_camera_timestamp ON recordings(camera_id, start_timestamp);
CREATE INDEX IF NOT EXISTS idx_recordings_timestamp ON recordings(start_timestamp);
CREATE INDEX IF NOT EXISTS idx_recordings_status ON recordings(status);
CREATE INDEX IF NOT EXISTS idx_recordings_hls_status ON recordings(hls_conversion_status, camera_id);

-- Recording segments for playback continuity
CREATE TABLE IF NOT EXISTS recording_segments (
    segment_id SERIAL PRIMARY KEY,
    recording_id INTEGER NOT NULL,
    segment_number INTEGER NOT NULL,
    segment_file_path TEXT NOT NULL,          -- MP4 source file for this segment
    segment_start_timestamp TIMESTAMP NOT NULL,
    segment_end_timestamp TIMESTAMP,
    segment_duration_seconds INTEGER,
    segment_size_bytes INTEGER,
    -- HLS output for this segment
    hls_ts_file_path TEXT,                    -- Corresponding .ts file path
    hls_ts_size_bytes INTEGER,                -- Size of .ts file
    hls_ts_duration_sec DOUBLE PRECISION,     -- Measured .ts duration (from GStreamer query)
    hls_conversion_status TEXT DEFAULT 'PENDING',
    hls_converted_at TIMESTAMP,
    FOREIGN KEY (recording_id) REFERENCES recordings(recording_id) ON DELETE CASCADE,
    UNIQUE(recording_id, segment_number),
    CONSTRAINT chk_seg_hls_status CHECK (hls_conversion_status IN ('PENDING', 'COMPLETED', 'FAILED'))
);

-- ============================================================================
-- SECTION 5: TIME SYNCHRONIZATION
-- ============================================================================

-- Time synchronization status and metrics
CREATE TABLE IF NOT EXISTS sync_status_log (
    log_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- Active sync method
    active_sync_method TEXT NOT NULL, -- PTP, NTP, GPS, SYSTEM
    sync_source TEXT, -- Master clock IP or GPS receiver ID
    
    -- PTP metrics
    ptp_state TEXT, -- INITIALIZING, LISTENING, UNCALIBRATED, SLAVE, MASTER, PASSIVE
    ptp_domain INTEGER,
    ptp_offset_ns INTEGER, -- Offset from PTP master in nanoseconds
    ptp_delay_ns INTEGER, -- Network delay
    ptp_jitter_ns DOUBLE PRECISION,
    ptp_master_clock_id TEXT,
    ptp_is_locked INTEGER DEFAULT 0,
    
    -- NTP metrics
    ntp_server TEXT,
    ntp_offset_ms DOUBLE PRECISION, -- Offset from NTP server in milliseconds
    ntp_jitter_ms DOUBLE PRECISION,
    ntp_stratum INTEGER,
    ntp_is_synced INTEGER DEFAULT 0,
    
    -- GPS metrics
    gps_timestamp TEXT,
    gps_fix_quality TEXT,
    gps_satellites INTEGER,
    gps_hdop DOUBLE PRECISION, -- Horizontal dilution of precision
    gps_time_valid INTEGER DEFAULT 0,
    
    -- Overall sync health
    overall_quality_ms DOUBLE PRECISION, -- Combined estimate of time accuracy
    sync_health_status TEXT DEFAULT 'GOOD', -- EXCELLENT, GOOD, DEGRADED, POOR, FAILED
    
    CONSTRAINT chk_sync_method CHECK (active_sync_method IN ('PTP', 'NTP', 'GPS', 'SYSTEM')),
    CONSTRAINT chk_sync_health CHECK (sync_health_status IN ('EXCELLENT', 'GOOD', 'DEGRADED', 'POOR', 'FAILED'))
);

-- GPS tracking breadcrumb trail
CREATE TABLE IF NOT EXISTS gps_track_log (
    track_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    altitude DOUBLE PRECISION,
    speed_kmh DOUBLE PRECISION,
    heading DOUBLE PRECISION, -- 0-360 degrees
    satellites INTEGER,
    fix_quality TEXT,
    hdop DOUBLE PRECISION,
    vdop DOUBLE PRECISION,
    pdop DOUBLE PRECISION,
    CONSTRAINT chk_gps_fix_log CHECK (fix_quality IN ('NO_FIX', '2D', '3D', 'DGPS'))
);

CREATE INDEX IF NOT EXISTS idx_gps_track_timestamp ON gps_track_log(timestamp);

-- ============================================================================
-- SECTION 6: EVENTS AND ALARMS
-- ============================================================================

-- Unified events table for all system events
CREATE TABLE IF NOT EXISTS events (
    event_id SERIAL PRIMARY KEY,
    event_type TEXT NOT NULL, -- ALARM, PANIC_BUTTON, ETBU, TAMPERING, RECORDING_FAILURE, SYSTEM_ERROR, etc.
    event_subtype TEXT, -- Specific classification
    severity TEXT DEFAULT 'INFO', -- INFO, WARNING, ERROR, CRITICAL, EMERGENCY
    source_device_type TEXT, -- CAMERA, MICROPHONE, GPS, RDAS, CHM, SYSTEM, PANIC_BUTTON, ETBU
    source_device_id INTEGER,
    
    -- Event details
    title TEXT NOT NULL,
    description TEXT,
    event_data TEXT, -- JSON format for additional data
    
    -- Location information
    camera_id INTEGER,
    coach_location TEXT,
    gps_latitude DOUBLE PRECISION,
    gps_longitude DOUBLE PRECISION,
    
    -- Status and handling
    status TEXT DEFAULT 'ACTIVE', -- ACTIVE, ACKNOWLEDGED, RESOLVED, DISMISSED
    is_acknowledged INTEGER DEFAULT 0,
    acknowledged_by TEXT,
    acknowledged_at TIMESTAMP,
    resolution_notes TEXT,
    resolved_at TIMESTAMP,
    
    -- Associated media
    snapshot_path TEXT,
    video_clip_path TEXT,
    audio_clip_path TEXT,
    
    -- Timestamps
    occurred_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_event_severity CHECK (severity IN ('INFO', 'WARNING', 'ERROR', 'CRITICAL', 'EMERGENCY')),
    CONSTRAINT chk_event_status CHECK (status IN ('ACTIVE', 'ACKNOWLEDGED', 'RESOLVED', 'DISMISSED'))
);

CREATE INDEX IF NOT EXISTS idx_events_type_time ON events(event_type, occurred_at);
CREATE INDEX IF NOT EXISTS idx_events_status ON events(status);
CREATE INDEX IF NOT EXISTS idx_events_severity ON events(severity);

-- Add FK from recordings.trigger_event_id -> events (deferred because events is defined after recordings)
ALTER TABLE recordings DROP CONSTRAINT IF EXISTS fk_recordings_trigger_event;
ALTER TABLE recordings ADD CONSTRAINT fk_recordings_trigger_event
    FOREIGN KEY (trigger_event_id) REFERENCES events(event_id) ON DELETE SET NULL;

-- Panic button activations
CREATE TABLE IF NOT EXISTS panic_button_events (
    panic_id SERIAL PRIMARY KEY,
    event_id INTEGER NOT NULL,
    button_id TEXT NOT NULL, -- Physical button identifier
    coach_number TEXT,
    coach_location TEXT,
    activation_timestamp TIMESTAMP NOT NULL,
    response_time_seconds INTEGER,
    responded_by TEXT,
    response_action TEXT, -- DISPATCHED_GUARD, CALLED_AUTHORITIES, FALSE_ALARM
    cameras_triggered TEXT, -- JSON array of camera IDs that were shown
    status TEXT DEFAULT 'ACTIVE',
    resolved_at TIMESTAMP,
    notes TEXT,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    CONSTRAINT chk_panic_status CHECK (status IN ('ACTIVE', 'RESPONDING', 'RESOLVED', 'FALSE_ALARM'))
);

-- ETBU (Emergency Talk Back Unit) events
CREATE TABLE IF NOT EXISTS etbu_events (
    etbu_id SERIAL PRIMARY KEY,
    event_id INTEGER NOT NULL,
    etbu_device_id TEXT NOT NULL,
    coach_number TEXT,
    call_start_timestamp TIMESTAMP NOT NULL,
    call_end_timestamp TIMESTAMP,
    call_duration_seconds INTEGER,
    answered_by TEXT,
    call_status TEXT DEFAULT 'INITIATED', -- INITIATED, RINGING, ANSWERED, DISMISSED, COMPLETED
    cameras_displayed TEXT, -- JSON array of camera IDs shown in cab
    audio_recording_path TEXT,
    notes TEXT,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    CONSTRAINT chk_etbu_status CHECK (call_status IN ('INITIATED', 'RINGING', 'ANSWERED', 'DISMISSED', 'COMPLETED'))
);

-- Recording failure alarms
CREATE TABLE IF NOT EXISTS recording_failure_alarms (
    alarm_id SERIAL PRIMARY KEY,
    event_id INTEGER NOT NULL,
    camera_id INTEGER,
    mic_id INTEGER,
    failure_type TEXT NOT NULL, -- NO_VIDEO, NO_AUDIO, STREAM_LOSS, DISK_FULL, WRITE_ERROR
    failure_timestamp TIMESTAMP NOT NULL,
    auto_recovered INTEGER DEFAULT 0,
    recovery_timestamp TIMESTAMP,
    manual_action_required INTEGER DEFAULT 0,
    acknowledged_by TEXT,
    acknowledged_at TIMESTAMP,
    resolution_notes TEXT,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    FOREIGN KEY (mic_id) REFERENCES microphones(mic_id) ON DELETE CASCADE,
    CONSTRAINT chk_failure_type CHECK (failure_type IN ('NO_VIDEO', 'NO_AUDIO', 'STREAM_LOSS', 'DISK_FULL', 'WRITE_ERROR', 'NETWORK_ERROR'))
);

-- ============================================================================
-- SECTION 7: FACE RECOGNITION
-- ============================================================================

-- Face detection and cropping for FRS (Facial Recognition System)
CREATE TABLE IF NOT EXISTS face_detections (
    face_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    detection_timestamp TIMESTAMP NOT NULL,
    
    -- Face image data
    face_image_path TEXT NOT NULL, -- Path to cropped face image
    face_thumbnail_path TEXT,
    original_frame_path TEXT,
    
    -- Face bounding box in original frame
    bbox_x INTEGER,
    bbox_y INTEGER,
    bbox_width INTEGER,
    bbox_height INTEGER,
    
    -- Face quality metrics
    quality_score DOUBLE PRECISION, -- 0.0 to 1.0
    is_frontal INTEGER DEFAULT 0,
    eye_distance_pixels INTEGER,
    face_size_pixels INTEGER,
    blur_score DOUBLE PRECISION,
    brightness_score DOUBLE PRECISION,
    meets_frs_standards INTEGER DEFAULT 0, -- Compliant with facial recognition standards
    
    -- Face attributes (if detected)
    estimated_age INTEGER,
    estimated_gender TEXT,
    has_glasses INTEGER DEFAULT 0,
    has_mask INTEGER DEFAULT 0,
    
    -- Location metadata
    gps_latitude DOUBLE PRECISION,
    gps_longitude DOUBLE PRECISION,
    coach_location TEXT,
    
    -- Status
    is_unique INTEGER DEFAULT 1, -- 0 if duplicate within time window
    duplicate_group_id INTEGER, -- Groups similar faces
    is_uploaded INTEGER DEFAULT 0,
    upload_timestamp TIMESTAMP,
    server_face_id TEXT, -- ID from face matching server
    
    -- MobileFaceNet 128-float L2-normalised embedding stored as raw IEEE-754
    -- little-endian bytes (128 * 4 = 512 bytes).  Written via parameterised
    -- query (see db_module.c) so no encoding issues arise.
    -- Read back:  SELECT encode(face_embedding, 'hex') FROM face_detections;
    face_embedding BYTEA,

    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE
);

-- Migration helper: add the column on existing deployments without data loss.
-- Safe to run repeatedly (ALTER TABLE IF NOT EXISTS ... ADD COLUMN IF NOT EXISTS
-- is Postgres 9.6+).
ALTER TABLE face_detections ADD COLUMN IF NOT EXISTS face_embedding BYTEA;
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS rec_rtsp_url TEXT;

CREATE INDEX IF NOT EXISTS idx_face_detection_camera_time ON face_detections(camera_id, detection_timestamp);
CREATE INDEX IF NOT EXISTS idx_face_detection_unique ON face_detections(is_unique, is_uploaded);

-- Face grouping for duplicate detection within time window
CREATE TABLE IF NOT EXISTS face_duplicate_groups (
    group_id SERIAL PRIMARY KEY,
    representative_face_id INTEGER NOT NULL, -- Primary face in this group
    face_count INTEGER DEFAULT 1,
    first_seen_timestamp TIMESTAMP,
    last_seen_timestamp TIMESTAMP,
    time_window_minutes INTEGER DEFAULT 30,
    similarity_threshold DOUBLE PRECISION DEFAULT 0.85,
    FOREIGN KEY (representative_face_id) REFERENCES face_detections(face_id) ON DELETE CASCADE
);

-- ============================================================================
-- SECTION 8: RDAS (Railway Driver Assistance System)
-- ============================================================================

-- RDAS device configuration
CREATE TABLE IF NOT EXISTS rdas_devices (
    rdas_id SERIAL PRIMARY KEY,
    device_name TEXT NOT NULL,
    ip_address TEXT NOT NULL,
    modbus_port INTEGER DEFAULT 502,
    usb_port TEXT,
    firmware_version TEXT,
    camera_id INTEGER, -- Internal camera monitoring driver
    status TEXT DEFAULT 'ACTIVE',
    last_heartbeat TIMESTAMP,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE SET NULL,
    CONSTRAINT chk_rdas_status CHECK (status IN ('ACTIVE', 'INACTIVE', 'FAULTY', 'MAINTENANCE'))
);

-- RDAS alertness monitoring events
CREATE TABLE IF NOT EXISTS rdas_alertness_events (
    rdas_event_id SERIAL PRIMARY KEY,
    rdas_id INTEGER NOT NULL,
    event_id INTEGER,
    
    -- Detection details
    detection_timestamp TIMESTAMP NOT NULL,
    alert_type TEXT NOT NULL, -- DROWSINESS, DISTRACTION, EYES_CLOSED, HEAD_DOWN, NO_FACE
    severity TEXT DEFAULT 'WARNING',
    confidence_score DOUBLE PRECISION,
    
    -- Driver state
    eyes_open_score DOUBLE PRECISION, -- 0.0 (closed) to 1.0 (open)
    head_pose_pitch DOUBLE PRECISION, -- degrees
    head_pose_yaw DOUBLE PRECISION, -- degrees
    attention_score DOUBLE PRECISION, -- 0.0 to 1.0
    
    -- Alert handling
    alert_triggered INTEGER DEFAULT 0,
    alert_acknowledged INTEGER DEFAULT 0,
    acknowledge_timestamp TIMESTAMP,
    acknowledge_method TEXT, -- BUTTON_PRESS, AUTO_RECOVERY
    
    -- Associated media
    snapshot_path TEXT,
    video_clip_path TEXT,
    
    duration_seconds INTEGER,
    resolved_timestamp TIMESTAMP,
    
    FOREIGN KEY (rdas_id) REFERENCES rdas_devices(rdas_id) ON DELETE CASCADE,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    CONSTRAINT chk_rdas_alert_type CHECK (alert_type IN ('DROWSINESS', 'DISTRACTION', 'EYES_CLOSED', 'HEAD_DOWN', 'NO_FACE', 'PHONE_USE')),
    CONSTRAINT chk_rdas_severity CHECK (severity IN ('INFO', 'WARNING', 'CRITICAL'))
);

CREATE INDEX IF NOT EXISTS idx_rdas_events_time ON rdas_alertness_events(detection_timestamp);

-- ============================================================================
-- SECTION 9: CHM (CRASH HARDENED MEMORY) OPERATIONS
-- ============================================================================

-- CHM device configuration (IEEE 1482.1)
CREATE TABLE IF NOT EXISTS chm_devices (
    chm_id SERIAL PRIMARY KEY,
    device_name TEXT NOT NULL,
    device_serial TEXT UNIQUE,
    interface_type TEXT DEFAULT 'ETHERNET', -- ETHERNET, USB
    ip_address TEXT,
    port INTEGER,
    capacity_gb INTEGER,
    status TEXT DEFAULT 'ACTIVE',
    last_write_timestamp TIMESTAMP,
    last_health_check TIMESTAMP,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT chk_chm_interface CHECK (interface_type IN ('ETHERNET', 'USB')),
    CONSTRAINT chk_chm_status CHECK (status IN ('ACTIVE', 'INACTIVE', 'FULL', 'FAULTY'))
);

-- CHM backup operations log
CREATE TABLE IF NOT EXISTS chm_backup_log (
    backup_id SERIAL PRIMARY KEY,
    chm_id INTEGER NOT NULL,
    recording_id INTEGER,
    event_id INTEGER, -- For critical events requiring immediate backup
    
    backup_timestamp TIMESTAMP NOT NULL,
    backup_type TEXT DEFAULT 'CONTINUOUS', -- CONTINUOUS, CRITICAL_EVENT, MANUAL
    
    -- Data details
    data_type TEXT NOT NULL, -- VIDEO, AUDIO, METADATA, EVENT_DATA
    source_file_path TEXT,
    backup_file_path TEXT,
    file_size_bytes INTEGER,
    
    -- Integrity
    checksum_algorithm TEXT DEFAULT 'SHA256',
    checksum_value TEXT,
    verification_status TEXT DEFAULT 'PENDING', -- PENDING, VERIFIED, FAILED
    
    -- Status
    backup_status TEXT DEFAULT 'IN_PROGRESS',
    completion_percentage DOUBLE PRECISION DEFAULT 0.0,
    error_message TEXT,
    completed_at TIMESTAMP,
    
    FOREIGN KEY (chm_id) REFERENCES chm_devices(chm_id) ON DELETE CASCADE,
    FOREIGN KEY (recording_id) REFERENCES recordings(recording_id) ON DELETE CASCADE,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_backup_type CHECK (backup_type IN ('CONTINUOUS', 'CRITICAL_EVENT', 'MANUAL')),
    CONSTRAINT chk_data_type CHECK (data_type IN ('VIDEO', 'AUDIO', 'METADATA', 'EVENT_DATA', 'SYSTEM_LOG')),
    CONSTRAINT chk_backup_status CHECK (backup_status IN ('IN_PROGRESS', 'COMPLETED', 'FAILED', 'CANCELLED')),
    CONSTRAINT chk_verification CHECK (verification_status IN ('PENDING', 'VERIFIED', 'FAILED'))
);

CREATE INDEX IF NOT EXISTS idx_chm_backup_timestamp ON chm_backup_log(backup_timestamp);

-- ============================================================================
-- SECTION 10: USER MANAGEMENT AND AUTHENTICATION
-- ============================================================================

-- User accounts with role-based access control (RBAC)
CREATE TABLE IF NOT EXISTS users (
    user_id SERIAL PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL, -- bcrypt or argon2
    full_name TEXT NOT NULL,
    email TEXT,
    phone TEXT,
    
    -- Role-based access
    role TEXT NOT NULL DEFAULT 'OPERATOR', -- ADMIN, OPERATOR, MAINTENANCE, VIEWER
    
    -- Account status
    is_active INTEGER DEFAULT 1,
    is_locked INTEGER DEFAULT 0,
    failed_login_attempts INTEGER DEFAULT 0,
    last_failed_login TIMESTAMP,
    password_expires_at TIMESTAMP,
    must_change_password INTEGER DEFAULT 0,
    
    -- Audit trail
    created_by TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_login_at TIMESTAMP,
    last_login_ip TEXT,
    
    CONSTRAINT chk_user_role CHECK (role IN ('ADMIN', 'OPERATOR', 'MAINTENANCE', 'VIEWER', 'API_CLIENT'))
);

-- Default admin user (password should be changed on first login)
-- Default password: Admin@123 (hashed with bcrypt)
INSERT INTO users (username, password_hash, full_name, role, must_change_password) VALUES
('admin', '$2b$12$LQv3c1yqBWVHxkd0LHAkCOYz6TtxMQJqhN8/LewY5xdeyKWM6wXu', 'System Administrator', 'ADMIN', 1)
ON CONFLICT (username) DO NOTHING;

-- User sessions for authentication
CREATE TABLE IF NOT EXISTS user_sessions (
    session_id TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL,
    token TEXT NOT NULL UNIQUE, -- JWT token
    device_info TEXT, -- User agent, device type
    ip_address TEXT,
    login_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_activity_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP NOT NULL,
    is_active INTEGER DEFAULT 1,
    logout_timestamp TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_sessions_token ON user_sessions(token);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON user_sessions(user_id, is_active);

-- User permissions (granular access control)
CREATE TABLE IF NOT EXISTS user_permissions (
    permission_id SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL,
    resource_type TEXT NOT NULL, -- CAMERA, RECORDING, SYSTEM_CONFIG, USER_MGMT, EXPORT
    resource_id INTEGER, -- NULL for all resources of type
    permission_level TEXT NOT NULL, -- READ, WRITE, DELETE, ADMIN
    granted_by TEXT,
    granted_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE,
    CONSTRAINT chk_resource_type CHECK (resource_type IN ('CAMERA', 'RECORDING', 'SYSTEM_CONFIG', 'USER_MGMT', 'EXPORT', 'EVENTS', 'ANALYTICS')),
    CONSTRAINT chk_permission_level CHECK (permission_level IN ('READ', 'WRITE', 'DELETE', 'ADMIN'))
);

-- ============================================================================
-- SECTION 11: AUDIT LOGGING
-- ============================================================================

-- Comprehensive audit log for all user actions
CREATE TABLE IF NOT EXISTS audit_log (
    audit_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- User information
    user_id INTEGER,
    username TEXT,
    user_role TEXT,
    session_id TEXT,
    
    -- Action details
    action_type TEXT NOT NULL, -- LOGIN, LOGOUT, VIEW, CREATE, UPDATE, DELETE, EXPORT, CONFIG_CHANGE
    action_category TEXT, -- USER_MGMT, CAMERA_MGMT, RECORDING, SYSTEM, EXPORT
    resource_type TEXT, -- CAMERA, RECORDING, USER, CONFIG, etc.
    resource_id INTEGER,
    resource_name TEXT,
    
    -- Action outcome
    action_status TEXT DEFAULT 'SUCCESS', -- SUCCESS, FAILED, PARTIAL
    error_message TEXT,
    
    -- Request details
    ip_address TEXT,
    user_agent TEXT,
    request_method TEXT, -- GET, POST, PUT, DELETE
    request_endpoint TEXT,
    
    -- Change tracking
    old_value TEXT, -- JSON format
    new_value TEXT, -- JSON format
    
    -- Additional context
    description TEXT,
    metadata TEXT, -- JSON format for additional data
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_action_type CHECK (action_type IN ('LOGIN', 'LOGOUT', 'VIEW', 'CREATE', 'UPDATE', 'DELETE', 'EXPORT', 'CONFIG_CHANGE', 'ALARM_ACK', 'SYSTEM_REBOOT')),
    CONSTRAINT chk_action_status CHECK (action_status IN ('SUCCESS', 'FAILED', 'PARTIAL'))
);

CREATE INDEX IF NOT EXISTS idx_audit_user_time ON audit_log(user_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_audit_action_type ON audit_log(action_type, timestamp);
CREATE INDEX IF NOT EXISTS idx_audit_resource ON audit_log(resource_type, resource_id);

-- System operation logs
CREATE TABLE IF NOT EXISTS system_logs (
    log_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    log_level TEXT NOT NULL, -- DEBUG, INFO, WARNING, ERROR, CRITICAL
    component TEXT NOT NULL, -- NVR_CORE, API_SERVER, VMS_DESKTOP, VMS_WEB, TIME_SYNC, GPS, etc.
    process_id INTEGER,
    thread_id INTEGER,
    
    -- Log message
    message TEXT NOT NULL,
    error_code TEXT,
    stack_trace TEXT,
    
    -- Context
    context_data TEXT, -- JSON format
    
    CONSTRAINT chk_log_level CHECK (log_level IN ('DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'))
);

CREATE INDEX IF NOT EXISTS idx_system_logs_level_time ON system_logs(log_level, timestamp);
CREATE INDEX IF NOT EXISTS idx_system_logs_component ON system_logs(component, timestamp);

-- ============================================================================
-- SECTION 12: STORAGE MANAGEMENT
-- ============================================================================

-- Storage devices status
CREATE TABLE IF NOT EXISTS storage_devices (
    storage_id SERIAL PRIMARY KEY,
    device_name TEXT NOT NULL,
    device_path TEXT NOT NULL UNIQUE, -- Mount point
    device_type TEXT NOT NULL, -- SSD, HDD, NVME, USB, NETWORK
    
    -- Capacity metrics
    total_capacity_gb DOUBLE PRECISION NOT NULL,
    used_capacity_gb DOUBLE PRECISION DEFAULT 0,
    free_capacity_gb DOUBLE PRECISION,
    usage_percentage DOUBLE PRECISION DEFAULT 0,
    
    -- Health metrics
    health_status TEXT DEFAULT 'HEALTHY', -- HEALTHY, WARNING, CRITICAL, FAILED
    temperature_celsius DOUBLE PRECISION,
    smart_status TEXT,
    bad_blocks_count INTEGER DEFAULT 0,
    read_error_rate DOUBLE PRECISION,
    write_error_rate DOUBLE PRECISION,
    
    -- Status
    is_mounted INTEGER DEFAULT 0,
    is_writable INTEGER DEFAULT 1,
    is_primary INTEGER DEFAULT 0, -- Primary storage device
    mount_timestamp TIMESTAMP,
    
    last_health_check TIMESTAMP,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_storage_type CHECK (device_type IN ('SSD', 'HDD', 'NVME', 'USB', 'NETWORK', 'CHM')),
    CONSTRAINT chk_storage_health CHECK (health_status IN ('HEALTHY', 'WARNING', 'CRITICAL', 'FAILED'))
);

-- Storage cleanup history (30-day retention circular buffer)
CREATE TABLE IF NOT EXISTS storage_cleanup_log (
    cleanup_id SERIAL PRIMARY KEY,
    cleanup_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    storage_id INTEGER NOT NULL,
    
    -- Cleanup metrics
    files_deleted INTEGER DEFAULT 0,
    space_freed_gb DOUBLE PRECISION DEFAULT 0,
    oldest_file_deleted_date TIMESTAMP,
    newest_file_deleted_date TIMESTAMP,
    
    -- Cleanup trigger
    trigger_reason TEXT, -- SCHEDULED, LOW_SPACE, RETENTION_POLICY, MANUAL
    threshold_reached_gb DOUBLE PRECISION,
    
    cleanup_duration_seconds INTEGER,
    status TEXT DEFAULT 'COMPLETED',
    error_message TEXT,
    
    FOREIGN KEY (storage_id) REFERENCES storage_devices(storage_id) ON DELETE CASCADE,
    CONSTRAINT chk_cleanup_trigger CHECK (trigger_reason IN ('SCHEDULED', 'LOW_SPACE', 'RETENTION_POLICY', 'MANUAL')),
    CONSTRAINT chk_cleanup_status CHECK (status IN ('IN_PROGRESS', 'COMPLETED', 'FAILED'))
);

-- ============================================================================
-- SECTION 13: SYSTEM HEALTH MONITORING
-- ============================================================================

-- System health metrics
CREATE TABLE IF NOT EXISTS system_health (
    health_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- CPU metrics
    cpu_usage_percent DOUBLE PRECISION,
    cpu_temperature_celsius DOUBLE PRECISION,
    cpu_frequency_mhz INTEGER,
    
    -- Memory metrics
    memory_total_mb INTEGER,
    memory_used_mb INTEGER,
    memory_free_mb INTEGER,
    memory_usage_percent DOUBLE PRECISION,
    swap_used_mb INTEGER,
    swap_usage_percent DOUBLE PRECISION,
    
    -- Network metrics
    network_rx_bytes_per_sec INTEGER,
    network_tx_bytes_per_sec INTEGER,
    network_packet_loss_percent DOUBLE PRECISION,
    active_connections INTEGER,
    
    -- Disk I/O
    disk_read_bytes_per_sec INTEGER,
    disk_write_bytes_per_sec INTEGER,
    disk_iops INTEGER,
    
    -- Process metrics
    active_processes INTEGER,
    nvr_process_cpu_percent DOUBLE PRECISION,
    nvr_process_memory_mb INTEGER,
    api_server_cpu_percent DOUBLE PRECISION,
    api_server_memory_mb INTEGER,
    
    -- Power metrics
    power_supply_voltage DOUBLE PRECISION,
    battery_percentage INTEGER,
    is_on_battery INTEGER DEFAULT 0,
    
    -- Overall system status
    overall_health_score DOUBLE PRECISION, -- 0-100
    health_status TEXT DEFAULT 'HEALTHY',
    
    CONSTRAINT chk_system_health CHECK (health_status IN ('HEALTHY', 'WARNING', 'CRITICAL', 'FAILED'))
);

CREATE INDEX IF NOT EXISTS idx_system_health_time ON system_health(timestamp);

-- Component health status
CREATE TABLE IF NOT EXISTS component_health (
    component_id SERIAL PRIMARY KEY,
    component_name TEXT NOT NULL UNIQUE, -- NVR_CORE, API_SERVER, TIME_SYNC, GPS, etc.
    component_type TEXT NOT NULL,
    
    -- Status
    status TEXT DEFAULT 'RUNNING', -- RUNNING, STOPPED, ERROR, RESTARTING
    health_status TEXT DEFAULT 'HEALTHY',
    
    -- Metrics
    uptime_seconds INTEGER,
    restart_count INTEGER DEFAULT 0,
    last_restart_timestamp TIMESTAMP,
    error_count INTEGER DEFAULT 0,
    last_error_timestamp TIMESTAMP,
    last_error_message TEXT,
    
    -- Process info
    process_id INTEGER,
    memory_usage_mb INTEGER,
    cpu_usage_percent DOUBLE PRECISION,
    
    last_heartbeat TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_component_status CHECK (status IN ('RUNNING', 'STOPPED', 'ERROR', 'RESTARTING', 'INITIALIZING')),
    CONSTRAINT chk_component_health CHECK (health_status IN ('HEALTHY', 'DEGRADED', 'FAILED'))
);

-- Network interface health
CREATE TABLE IF NOT EXISTS network_interfaces (
    interface_id SERIAL PRIMARY KEY,
    interface_name TEXT NOT NULL UNIQUE, -- eth0, wlan0, cellular0, etc.
    interface_type TEXT NOT NULL, -- ETHERNET, WIFI, CELLULAR_4G, CELLULAR_5G
    
    -- Status
    is_up INTEGER DEFAULT 0,
    is_connected INTEGER DEFAULT 0,
    ip_address TEXT,
    mac_address TEXT,
    
    -- Cellular specific
    sim_slot INTEGER, -- 1 or 2 for dual SIM
    signal_strength_dbm INTEGER,
    network_operator TEXT,
    connection_type TEXT, -- 4G, 5G, LTE
    
    -- WiFi specific
    ssid TEXT,
    wifi_channel INTEGER,
    wifi_frequency_ghz DOUBLE PRECISION,
    
    -- Metrics
    rx_bytes INTEGER DEFAULT 0,
    tx_bytes INTEGER DEFAULT 0,
    rx_errors INTEGER DEFAULT 0,
    tx_errors INTEGER DEFAULT 0,
    latency_ms INTEGER,
    
    last_connected_at TIMESTAMP,
    last_health_check TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_interface_type CHECK (interface_type IN ('ETHERNET', 'WIFI', 'CELLULAR_4G', 'CELLULAR_5G', 'BLUETOOTH'))
);

-- ============================================================================
-- SECTION 14: DATA EXPORT AND DOWNLOAD
-- ============================================================================

-- Export/download requests
CREATE TABLE IF NOT EXISTS export_requests (
    export_id SERIAL PRIMARY KEY,
    request_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- User information
    requested_by_user_id INTEGER NOT NULL,
    requested_by_username TEXT,
    
    -- Export criteria
    export_type TEXT NOT NULL, -- VIDEO, AUDIO, METADATA, EVENTS, SYSTEM_LOGS
    camera_ids TEXT, -- JSON array
    start_timestamp TIMESTAMP,
    end_timestamp TIMESTAMP,
    event_ids TEXT, -- JSON array for specific events
    
    -- Export format
    video_format TEXT DEFAULT 'MP4', -- MP4, AVI, MKV
    video_quality TEXT DEFAULT 'ORIGINAL', -- ORIGINAL, HIGH, MEDIUM, LOW
    include_audio INTEGER DEFAULT 1,
    include_metadata INTEGER DEFAULT 1,
    include_watermark INTEGER DEFAULT 1,
    
    -- Output details
    export_path TEXT,
    total_file_size_bytes INTEGER,
    file_count INTEGER,
    
    -- Status
    status TEXT DEFAULT 'PENDING', -- PENDING, IN_PROGRESS, COMPLETED, FAILED, CANCELLED
    progress_percentage DOUBLE PRECISION DEFAULT 0.0,
    estimated_completion_time TIMESTAMP,
    
    -- Completion
    completed_at TIMESTAMP,
    download_expiry_timestamp TIMESTAMP,
    is_downloaded INTEGER DEFAULT 0,
    downloaded_at TIMESTAMP,
    download_count INTEGER DEFAULT 0,
    
    error_message TEXT,
    
    FOREIGN KEY (requested_by_user_id) REFERENCES users(user_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_export_type CHECK (export_type IN ('VIDEO', 'AUDIO', 'METADATA', 'EVENTS', 'SYSTEM_LOGS', 'HEALTH_REPORT')),
    CONSTRAINT chk_export_status CHECK (status IN ('PENDING', 'IN_PROGRESS', 'COMPLETED', 'FAILED', 'CANCELLED')),
    CONSTRAINT chk_video_format CHECK (video_format IN ('MP4', 'AVI', 'MKV', 'RAW')),
    CONSTRAINT chk_video_quality CHECK (video_quality IN ('ORIGINAL', 'HIGH', 'MEDIUM', 'LOW'))
);

CREATE INDEX IF NOT EXISTS idx_export_user_time ON export_requests(requested_by_user_id, request_timestamp);
CREATE INDEX IF NOT EXISTS idx_export_status ON export_requests(status);

-- HHT (Hand-Held Terminal) download sessions
CREATE TABLE IF NOT EXISTS hht_download_sessions (
    session_id SERIAL PRIMARY KEY,
    hht_device_id TEXT NOT NULL,
    hht_serial_number TEXT,
    
    -- User authentication
    user_id INTEGER NOT NULL,
    username TEXT,
    auth_timestamp TIMESTAMP,
    
    -- Connection details
    connection_type TEXT NOT NULL, -- USB, ETHERNET, WIFI
    connection_established_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    connection_terminated_at TIMESTAMP,
    
    -- Download details
    download_start_timestamp TIMESTAMP,
    download_end_timestamp TIMESTAMP,
    total_data_transferred_gb DOUBLE PRECISION DEFAULT 0,
    
    -- Session status
    session_status TEXT DEFAULT 'ACTIVE', -- ACTIVE, COMPLETED, FAILED, TIMEOUT
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_hht_connection CHECK (connection_type IN ('USB', 'ETHERNET', 'WIFI')),
    CONSTRAINT chk_hht_session_status CHECK (session_status IN ('ACTIVE', 'COMPLETED', 'FAILED', 'TIMEOUT'))
);

-- HHT download history
CREATE TABLE IF NOT EXISTS hht_download_history (
    download_id SERIAL PRIMARY KEY,
    session_id INTEGER NOT NULL,
    recording_id INTEGER,
    
    file_path TEXT NOT NULL,
    file_name TEXT NOT NULL,
    file_size_bytes INTEGER,
    file_type TEXT, -- VIDEO, AUDIO, LOG, METADATA
    
    download_start_timestamp TIMESTAMP,
    download_completed_timestamp TIMESTAMP,
    download_duration_seconds INTEGER,
    transfer_rate_mbps DOUBLE PRECISION,
    
    -- Integrity verification
    checksum_algorithm TEXT DEFAULT 'SHA256',
    source_checksum TEXT,
    verified_checksum TEXT,
    is_verified INTEGER DEFAULT 0,
    
    status TEXT DEFAULT 'IN_PROGRESS',
    error_message TEXT,
    
    FOREIGN KEY (session_id) REFERENCES hht_download_sessions(session_id) ON DELETE CASCADE,
    FOREIGN KEY (recording_id) REFERENCES recordings(recording_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_hht_file_type CHECK (file_type IN ('VIDEO', 'AUDIO', 'LOG', 'METADATA', 'EVENT')),
    CONSTRAINT chk_hht_download_status CHECK (status IN ('PENDING', 'IN_PROGRESS', 'COMPLETED', 'FAILED', 'CANCELLED'))
);

-- ============================================================================
-- SECTION 15: ANALYTICS AND REPORTS
-- ============================================================================

-- Motion detection events
CREATE TABLE IF NOT EXISTS motion_detection_events (
    motion_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    detection_timestamp TIMESTAMP NOT NULL,
    
    -- Motion details
    motion_intensity DOUBLE PRECISION, -- 0.0 to 1.0
    motion_area_percent DOUBLE PRECISION, -- Percentage of frame with motion
    duration_seconds INTEGER,
    
    -- Motion region (bounding box)
    region_x INTEGER,
    region_y INTEGER,
    region_width INTEGER,
    region_height INTEGER,
    
    -- Associated recording
    recording_id INTEGER,
    snapshot_path TEXT,
    
    -- Status
    triggered_recording INTEGER DEFAULT 0,
    triggered_alert INTEGER DEFAULT 0,
    
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    FOREIGN KEY (recording_id) REFERENCES recordings(recording_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_motion_camera_time ON motion_detection_events(camera_id, detection_timestamp);

-- System statistics (aggregated daily)
CREATE TABLE IF NOT EXISTS daily_statistics (
    stat_id SERIAL PRIMARY KEY,
    stat_date DATE NOT NULL UNIQUE,
    
    -- Recording statistics
    total_recording_hours DOUBLE PRECISION,
    total_storage_used_gb DOUBLE PRECISION,
    recordings_created INTEGER,
    recordings_deleted INTEGER,
    
    -- Camera statistics
    camera_uptime_percent DOUBLE PRECISION,
    camera_failures INTEGER,
    tampering_events INTEGER,
    
    -- Event statistics
    total_events INTEGER,
    panic_button_activations INTEGER,
    etbu_calls INTEGER,
    rdas_alerts INTEGER,
    recording_failures INTEGER,
    
    -- Face detection
    faces_detected INTEGER,
    unique_faces INTEGER,
    faces_uploaded INTEGER,
    
    -- System health
    avg_cpu_usage_percent DOUBLE PRECISION,
    avg_memory_usage_percent DOUBLE PRECISION,
    avg_storage_usage_percent DOUBLE PRECISION,
    system_reboots INTEGER,
    
    -- Network
    total_data_uploaded_gb DOUBLE PRECISION,
    total_data_downloaded_gb DOUBLE PRECISION,
    network_downtime_minutes INTEGER,
    
    generated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- ============================================================================
-- SECTION 16: SOFTWARE UPDATES
-- ============================================================================

-- Software update packages
CREATE TABLE IF NOT EXISTS update_packages (
    package_id SERIAL PRIMARY KEY,
    package_name TEXT NOT NULL,
    package_version TEXT NOT NULL,
    component TEXT NOT NULL, -- NVR_CORE, API_SERVER, VMS_DESKTOP, VMS_WEB, CAMERA_FIRMWARE
    
    -- Package details
    package_url TEXT,
    package_file_path TEXT,
    package_size_bytes INTEGER,
    package_checksum TEXT,
    checksum_algorithm TEXT DEFAULT 'SHA256',
    
    -- Update metadata
    release_date DATE,
    is_critical INTEGER DEFAULT 0,
    is_security_update INTEGER DEFAULT 0,
    requires_reboot INTEGER DEFAULT 0,
    
    -- Compatibility
    min_version_required TEXT,
    compatible_hardware TEXT, -- JSON array
    
    -- Status
    status TEXT DEFAULT 'AVAILABLE', -- AVAILABLE, DOWNLOADING, DOWNLOADED, INSTALLING, INSTALLED, FAILED
    
    release_notes TEXT,
    downloaded_at TIMESTAMP,
    installed_at TIMESTAMP,
    
    CONSTRAINT chk_update_component CHECK (component IN ('NVR_CORE', 'API_SERVER', 'VMS_DESKTOP', 'VMS_WEB', 'CAMERA_FIRMWARE', 'SYSTEM_FIRMWARE')),
    CONSTRAINT chk_update_status CHECK (status IN ('AVAILABLE', 'DOWNLOADING', 'DOWNLOADED', 'INSTALLING', 'INSTALLED', 'FAILED', 'ROLLED_BACK'))
);

-- Update installation history
CREATE TABLE IF NOT EXISTS update_history (
    update_id SERIAL PRIMARY KEY,
    package_id INTEGER NOT NULL,
    
    -- Installation details
    install_started_at TIMESTAMP,
    install_completed_at TIMESTAMP,
    install_duration_seconds INTEGER,
    
    previous_version TEXT,
    new_version TEXT,
    
    -- Status
    installation_status TEXT DEFAULT 'IN_PROGRESS',
    
    -- User information
    initiated_by_user_id INTEGER,
    initiated_by_username TEXT,
    
    -- Rollback support
    can_rollback INTEGER DEFAULT 1,
    rolled_back INTEGER DEFAULT 0,
    rollback_timestamp TIMESTAMP,
    rollback_reason TEXT,
    
    error_message TEXT,
    installation_log TEXT,
    
    FOREIGN KEY (package_id) REFERENCES update_packages(package_id) ON DELETE CASCADE,
    FOREIGN KEY (initiated_by_user_id) REFERENCES users(user_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_install_status CHECK (installation_status IN ('IN_PROGRESS', 'COMPLETED', 'FAILED', 'ROLLED_BACK'))
);

-- ============================================================================
-- SECTION 17: NOTIFICATION SETTINGS
-- ============================================================================

-- Notification configuration
CREATE TABLE IF NOT EXISTS notification_settings (
    setting_id SERIAL PRIMARY KEY,
    user_id INTEGER,
    
    -- Notification channels
    enable_email INTEGER DEFAULT 0,
    enable_sms INTEGER DEFAULT 0,
    enable_push INTEGER DEFAULT 1,
    enable_websocket INTEGER DEFAULT 1,
    
    -- Email settings
    email_address TEXT,
    
    -- SMS settings
    phone_number TEXT,
    
    -- Event type subscriptions (JSON array)
    subscribed_event_types TEXT, -- ['PANIC_BUTTON', 'TAMPERING', 'RECORDING_FAILURE', etc.]
    
    -- Severity filter
    min_severity_level TEXT DEFAULT 'WARNING', -- INFO, WARNING, ERROR, CRITICAL, EMERGENCY
    
    -- Quiet hours
    quiet_hours_enabled INTEGER DEFAULT 0,
    quiet_hours_start TIME,
    quiet_hours_end TIME,
    
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_min_severity CHECK (min_severity_level IN ('INFO', 'WARNING', 'ERROR', 'CRITICAL', 'EMERGENCY'))
);

-- Notification delivery log
CREATE TABLE IF NOT EXISTS notification_log (
    notification_id SERIAL PRIMARY KEY,
    user_id INTEGER,
    event_id INTEGER,
    
    notification_type TEXT NOT NULL, -- EMAIL, SMS, PUSH, WEBSOCKET
    notification_title TEXT NOT NULL,
    notification_message TEXT NOT NULL,
    
    -- Delivery details
    sent_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    delivery_status TEXT DEFAULT 'PENDING', -- PENDING, SENT, DELIVERED, FAILED
    delivery_timestamp TIMESTAMP,
    read_timestamp TIMESTAMP,
    
    -- Channel-specific details
    recipient_address TEXT, -- Email or phone number
    error_message TEXT,
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_notification_type CHECK (notification_type IN ('EMAIL', 'SMS', 'PUSH', 'WEBSOCKET', 'IN_APP')),
    CONSTRAINT chk_delivery_status CHECK (delivery_status IN ('PENDING', 'SENT', 'DELIVERED', 'FAILED', 'READ'))
);

-- ============================================================================
-- SECTION 18: VIEWS FOR COMMON QUERIES
-- ============================================================================

-- View: Active cameras with latest health status
CREATE OR REPLACE VIEW v_active_cameras AS
SELECT
    c.camera_id,
    c.camera_name,
    c.camera_type,
    c.ip_address,
    c.rtsp_url,
    c.video_codec,
    c.resolution_width,
    c.resolution_height,
    c.target_fps,
    c.location_description,
    c.rec_output_dir,
    c.hls_output_dir,
    c.hls_playlist_url,
    c.status AS camera_status,
    c.ptz_supported,
    c.audio_supported,
    ch.is_online,
    ch.is_recording,
    ch.frame_rate_actual,
    ch.timestamp AS last_health_check,
    c.is_ptp_synced,
    c.is_ntp_synced
FROM cameras c
LEFT JOIN camera_health ch ON c.camera_id = ch.camera_id
    AND ch.health_id = (
        SELECT health_id FROM camera_health
        WHERE camera_id = c.camera_id
        ORDER BY timestamp DESC LIMIT 1
    )
WHERE c.status = 'ACTIVE';

-- View: Current recording status
CREATE OR REPLACE VIEW v_current_recordings AS
SELECT 
    r.recording_id,
    r.camera_id,
    c.camera_name,
    r.start_timestamp,
    r.duration_seconds,
    r.file_size_bytes,
    r.has_audio,
    r.sync_quality_ms,
    r.status,
    r.recording_mode
FROM recordings r
INNER JOIN cameras c ON r.camera_id = c.camera_id
WHERE r.status = 'RECORDING'
ORDER BY r.start_timestamp DESC;

-- View: Active events and alarms
CREATE OR REPLACE VIEW v_active_events AS
SELECT 
    e.event_id,
    e.event_type,
    e.severity,
    e.title,
    e.occurred_at,
    e.status,
    e.is_acknowledged,
    c.camera_name,
    e.gps_latitude,
    e.gps_longitude
FROM events e
LEFT JOIN cameras c ON e.camera_id = c.camera_id
WHERE e.status IN ('ACTIVE', 'ACKNOWLEDGED')
ORDER BY e.severity DESC, e.occurred_at DESC;

-- View: Storage summary
CREATE OR REPLACE VIEW v_storage_summary AS
SELECT 
    storage_id,
    device_name,
    device_type,
    total_capacity_gb,
    used_capacity_gb,
    free_capacity_gb,
    usage_percentage,
    health_status,
    is_mounted
FROM storage_devices
WHERE is_mounted = 1
ORDER BY is_primary DESC, usage_percentage DESC;

-- View: System health overview
CREATE OR REPLACE VIEW v_system_health_overview AS
SELECT 
    sh.timestamp,
    sh.cpu_usage_percent,
    sh.memory_usage_percent,
    sh.overall_health_score,
    sh.health_status,
    COUNT(DISTINCT c.camera_id) as total_cameras,
    SUM(CASE WHEN ch.is_online = 1 THEN 1 ELSE 0 END) as online_cameras,
    SUM(CASE WHEN ch.is_recording = 1 THEN 1 ELSE 0 END) as recording_cameras
FROM system_health sh
CROSS JOIN cameras c
LEFT JOIN camera_health ch ON c.camera_id = ch.camera_id
    AND ch.health_id = (
        SELECT health_id FROM camera_health 
        WHERE camera_id = c.camera_id 
        ORDER BY timestamp DESC LIMIT 1
    )
WHERE sh.health_id = (SELECT health_id FROM system_health ORDER BY timestamp DESC LIMIT 1)
GROUP BY sh.health_id;

-- ============================================================================
-- SECTION 19: TRIGGERS FOR AUTOMATED TASKS
-- ============================================================================

-- Trigger: Update camera timestamp on health update

CREATE OR REPLACE FUNCTION fn_trg_update_camera_last_seen() RETURNS TRIGGER AS $$
BEGIN
UPDATE cameras 
    SET last_seen_at = NEW.timestamp,
        updated_at = NOW()
    WHERE camera_id = NEW.camera_id;
    RETURN NULL;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_update_camera_last_seen ON camera_health;
CREATE TRIGGER trg_update_camera_last_seen
AFTER INSERT ON camera_health
FOR EACH ROW
EXECUTE PROCEDURE fn_trg_update_camera_last_seen();


-- Trigger: Update recording on completion

CREATE OR REPLACE FUNCTION fn_trg_finalize_recording() RETURNS TRIGGER AS $$
BEGIN
UPDATE recordings
    SET end_timestamp = NOW(),
        duration_seconds = EXTRACT(EPOCH FROM (NOW() - start_timestamp))::INTEGER,
        updated_at = NOW()
    WHERE recording_id = NEW.recording_id;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_finalize_recording ON recordings;
CREATE TRIGGER trg_finalize_recording
AFTER UPDATE OF status ON recordings
FOR EACH ROW
WHEN (NEW.status = 'COMPLETED' AND OLD.status = 'RECORDING')
EXECUTE PROCEDURE fn_trg_finalize_recording();


-- Trigger: Auto-create event for panic button

CREATE OR REPLACE FUNCTION fn_trg_panic_button_event() RETURNS TRIGGER AS $$
BEGIN
INSERT INTO events (
        event_type,
        event_subtype,
        severity,
        source_device_type,
        title,
        description,
        status,
        occurred_at
    ) VALUES (
        'ALARM',
        'PANIC_BUTTON',
        'EMERGENCY',
        'PANIC_BUTTON',
        'Panic Button Activated',
        'Panic button ' || NEW.button_id || ' activated in coach ' || COALESCE(NEW.coach_number, 'Unknown'),
        'ACTIVE',
        NEW.activation_timestamp
    );
    
    UPDATE panic_button_events
    SET event_id = LASTVAL()
    WHERE panic_id = NEW.panic_id;
    RETURN NULL;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_panic_button_event ON panic_button_events;
CREATE TRIGGER trg_panic_button_event
AFTER INSERT ON panic_button_events
FOR EACH ROW
EXECUTE PROCEDURE fn_trg_panic_button_event();


-- Trigger: Log user login to audit

CREATE OR REPLACE FUNCTION fn_trg_audit_user_login() RETURNS TRIGGER AS $$
BEGIN
INSERT INTO audit_log (
        user_id,
        username,
        action_type,
        action_category,
        action_status,
        ip_address,
        description
    )
    SELECT 
        NEW.user_id,
        u.username,
        'LOGIN',
        'USER_MGMT',
        'SUCCESS',
        NEW.ip_address,
        'User logged in from ' || COALESCE(NEW.device_info, 'Unknown device')
    FROM users u
    WHERE u.user_id = NEW.user_id;
    RETURN NULL;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_audit_user_login ON user_sessions;
CREATE TRIGGER trg_audit_user_login
AFTER INSERT ON user_sessions
FOR EACH ROW
EXECUTE PROCEDURE fn_trg_audit_user_login();


-- Trigger: Update storage device metrics

CREATE OR REPLACE FUNCTION fn_trg_update_storage_metrics() RETURNS TRIGGER AS $$
BEGIN
UPDATE storage_devices
    SET used_capacity_gb = used_capacity_gb + (NEW.file_size_bytes / 1073741824.0),
        free_capacity_gb = total_capacity_gb - used_capacity_gb,
        usage_percentage = (used_capacity_gb / total_capacity_gb) * 100,
        updated_at = NOW()
    WHERE device_path = substr(NEW.file_path, 1, position('/' IN NEW.file_path) - 1);
    RETURN NULL;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_update_storage_metrics ON recordings;
CREATE TRIGGER trg_update_storage_metrics
AFTER INSERT ON recordings
FOR EACH ROW
EXECUTE PROCEDURE fn_trg_update_storage_metrics();


-- ============================================================================
-- SECTION 20: INDEXES FOR PERFORMANCE OPTIMIZATION
-- ============================================================================

-- Additional indexes for frequently queried tables
CREATE INDEX IF NOT EXISTS idx_cameras_status ON cameras(status);
CREATE INDEX IF NOT EXISTS idx_cameras_type ON cameras(camera_type);
CREATE INDEX IF NOT EXISTS idx_recordings_camera_status ON recordings(camera_id, status);
CREATE INDEX IF NOT EXISTS idx_events_camera_time ON events(camera_id, occurred_at);
CREATE INDEX IF NOT EXISTS idx_sync_log_timestamp ON sync_status_log(timestamp);
CREATE INDEX IF NOT EXISTS idx_system_health_timestamp ON system_health(timestamp);
CREATE INDEX IF NOT EXISTS idx_component_health_status ON component_health(status, health_status);

-- ============================================================================
-- SECTION 21: CONFIGURATION PRESETS
-- ============================================================================

-- Recording quality presets
CREATE TABLE IF NOT EXISTS recording_presets (
    preset_id SERIAL PRIMARY KEY,
    preset_name TEXT NOT NULL UNIQUE,
    description TEXT,
    
    -- Video settings
    video_codec TEXT DEFAULT 'H.265',
    resolution_width INTEGER DEFAULT 1920,
    resolution_height INTEGER DEFAULT 1080,
    fps INTEGER DEFAULT 25,
    video_bitrate_kbps INTEGER DEFAULT 4000,
    gop_size INTEGER DEFAULT 50,
    
    -- Audio settings
    enable_audio INTEGER DEFAULT 1,
    audio_codec TEXT DEFAULT 'AAC',
    audio_sample_rate INTEGER DEFAULT 48000,
    audio_bitrate_kbps INTEGER DEFAULT 128,
    
    -- Recording settings
    segment_duration_minutes INTEGER DEFAULT 10,
    enable_motion_detection INTEGER DEFAULT 0,
    motion_sensitivity DOUBLE PRECISION DEFAULT 0.5,
    
    -- Metadata
    enable_gps_overlay INTEGER DEFAULT 1,
    enable_timestamp_overlay INTEGER DEFAULT 1,
    enable_watermark INTEGER DEFAULT 1,
    
    is_default INTEGER DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_preset_codec CHECK (video_codec IN ('H.264', 'H.265', 'H.265+'))
);

-- Default presets
INSERT INTO recording_presets (preset_name, description, fps, video_bitrate_kbps, is_default) VALUES
('High Quality (Interior)', 'For interior cameras - 1080p@25fps', 25, 4000, 1),
('High Quality (Exterior)', 'For exterior cameras - 1080p@45fps', 45, 6000, 0),
('Medium Quality', 'Balanced quality and storage - 1080p@25fps', 25, 2500, 0),
('Low Quality', 'For low bandwidth - 720p@15fps', 15, 1500, 0)
ON CONFLICT DO NOTHING;

-- ============================================================================
-- SECTION 22: MAINTENANCE SCHEDULES
-- ============================================================================

-- Scheduled maintenance tasks
CREATE TABLE IF NOT EXISTS maintenance_schedules (
    schedule_id SERIAL PRIMARY KEY,
    task_name TEXT NOT NULL,
    task_type TEXT NOT NULL, -- CLEANUP, BACKUP, HEALTH_CHECK, UPDATE_CHECK, LOG_ROTATION
    
    -- Schedule definition
    schedule_type TEXT NOT NULL, -- DAILY, WEEKLY, MONTHLY, INTERVAL
    schedule_time TIME, -- For DAILY
    schedule_day_of_week INTEGER, -- 0-6 for WEEKLY
    schedule_day_of_month INTEGER, -- 1-31 for MONTHLY
    interval_minutes INTEGER, -- For INTERVAL
    
    -- Task configuration
    task_config TEXT, -- JSON format
    
    -- Status
    is_enabled INTEGER DEFAULT 1,
    last_run_timestamp TIMESTAMP,
    next_run_timestamp TIMESTAMP,
    run_count INTEGER DEFAULT 0,
    last_run_status TEXT,
    last_run_duration_seconds INTEGER,
    
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_task_type CHECK (task_type IN ('CLEANUP', 'BACKUP', 'HEALTH_CHECK', 'UPDATE_CHECK', 'LOG_ROTATION', 'SYNC_CHECK')),
    CONSTRAINT chk_schedule_type CHECK (schedule_type IN ('DAILY', 'WEEKLY', 'MONTHLY', 'INTERVAL', 'MANUAL'))
);

-- Default maintenance schedules
INSERT INTO maintenance_schedules (task_name, task_type, schedule_type, schedule_time, is_enabled) VALUES
('Daily Storage Cleanup', 'CLEANUP', 'DAILY', '02:00:00', 1),
('Daily Health Check', 'HEALTH_CHECK', 'DAILY', '03:00:00', 1),
('Daily Log Rotation', 'LOG_ROTATION', 'DAILY', '04:00:00', 1),
('Weekly Update Check', 'UPDATE_CHECK', 'WEEKLY', '05:00:00', 1),
('Daily CHM Backup', 'BACKUP', 'DAILY', '01:00:00', 1)
ON CONFLICT DO NOTHING;

-- Maintenance execution log
CREATE TABLE IF NOT EXISTS maintenance_execution_log (
    execution_id SERIAL PRIMARY KEY,
    schedule_id INTEGER NOT NULL,
    
    execution_start TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    execution_end TIMESTAMP,
    execution_duration_seconds INTEGER,
    
    -- Results
    status TEXT DEFAULT 'IN_PROGRESS', -- IN_PROGRESS, COMPLETED, FAILED, PARTIAL
    items_processed INTEGER DEFAULT 0,
    items_failed INTEGER DEFAULT 0,
    
    -- Details
    execution_summary TEXT,
    error_message TEXT,
    execution_log TEXT,
    
    FOREIGN KEY (schedule_id) REFERENCES maintenance_schedules(schedule_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_execution_status CHECK (status IN ('IN_PROGRESS', 'COMPLETED', 'FAILED', 'PARTIAL', 'CANCELLED'))
);

-- ============================================================================
-- SECTION 23: API RATE LIMITING
-- ============================================================================

-- API rate limit configuration
CREATE TABLE IF NOT EXISTS api_rate_limits (
    limit_id SERIAL PRIMARY KEY,
    user_id INTEGER,
    user_role TEXT,
    endpoint_pattern TEXT, -- e.g., '/api/recordings/*', '/api/cameras/*'
    
    -- Limit configuration
    requests_per_minute INTEGER DEFAULT 60,
    requests_per_hour INTEGER DEFAULT 1000,
    requests_per_day INTEGER DEFAULT 10000,
    
    -- Burst allowance
    burst_size INTEGER DEFAULT 10,
    
    is_enabled INTEGER DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE,
    
    UNIQUE(user_id, endpoint_pattern)
);

-- API request log (for rate limiting tracking)
CREATE TABLE IF NOT EXISTS api_request_log (
    request_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    user_id INTEGER,
    session_id TEXT,
    ip_address TEXT,
    
    -- Request details
    http_method TEXT,
    endpoint TEXT,
    query_params TEXT,
    
    -- Response
    response_status INTEGER,
    response_time_ms INTEGER,
    response_size_bytes INTEGER,
    
    -- Rate limiting
    rate_limit_hit INTEGER DEFAULT 0,
    
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_api_requests_user_time ON api_request_log(user_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_api_requests_endpoint ON api_request_log(endpoint, timestamp);

-- ============================================================================
-- SECTION 24: DEVICE PAIRING AND DISCOVERY
-- ============================================================================

-- Discovered devices (ONVIF auto-discovery)
CREATE TABLE IF NOT EXISTS discovered_devices (
    discovery_id SERIAL PRIMARY KEY,
    discovered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- Device identification
    device_type TEXT NOT NULL, -- CAMERA, MICROPHONE, RDAS
    onvif_device_id TEXT,
    mac_address TEXT,
    ip_address TEXT NOT NULL,
    hostname TEXT,
    
    -- Device capabilities
    manufacturer TEXT,
    model TEXT,
    firmware_version TEXT,
    hardware_id TEXT,
    serial_number TEXT,
    
    -- ONVIF profiles
    supports_profile_s INTEGER DEFAULT 0,
    supports_profile_t INTEGER DEFAULT 0,
    rtsp_port INTEGER DEFAULT 554,
    
    -- Discovery metadata
    discovery_method TEXT, -- ONVIF, MDNS, MANUAL, DHCP
    is_configured INTEGER DEFAULT 0,
    paired_device_id INTEGER, -- References cameras/microphones/rdas_devices
    
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_discovery_device_type CHECK (device_type IN ('CAMERA', 'MICROPHONE', 'RDAS', 'UNKNOWN')),
    CONSTRAINT chk_discovery_method CHECK (discovery_method IN ('ONVIF', 'MDNS', 'MANUAL', 'DHCP'))
);

CREATE INDEX IF NOT EXISTS idx_discovered_ip ON discovered_devices(ip_address);
CREATE INDEX IF NOT EXISTS idx_discovered_configured ON discovered_devices(is_configured);

-- ============================================================================
-- SECTION 25: BANDWIDTH MANAGEMENT
-- ============================================================================

-- Bandwidth allocation profiles
CREATE TABLE IF NOT EXISTS bandwidth_profiles (
    profile_id SERIAL PRIMARY KEY,
    profile_name TEXT NOT NULL UNIQUE,
    description TEXT,
    
    -- Upload bandwidth (to cloud/server)
    max_upload_mbps DOUBLE PRECISION,
    priority_upload_events TEXT, -- JSON array of event types
    
    -- Download bandwidth
    max_download_mbps DOUBLE PRECISION,
    
    -- Per-camera limits
    max_bitrate_per_camera_kbps INTEGER,
    
    -- Cellular data limits
    cellular_data_limit_gb INTEGER,
    cellular_data_warning_threshold_percent INTEGER DEFAULT 80,
    
    -- Schedule-based throttling
    throttle_during_hours INTEGER DEFAULT 0,
    throttle_start_time TIME,
    throttle_end_time TIME,
    throttle_reduction_percent INTEGER DEFAULT 50,
    
    is_active INTEGER DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_throttle_percent CHECK (throttle_reduction_percent BETWEEN 0 AND 100)
);

-- Bandwidth usage tracking
CREATE TABLE IF NOT EXISTS bandwidth_usage_log (
    usage_id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- Interface-specific usage
    interface_name TEXT NOT NULL,
    interface_type TEXT,
    
    -- Traffic metrics
    upload_bytes INTEGER DEFAULT 0,
    download_bytes INTEGER DEFAULT 0,
    upload_mbps DOUBLE PRECISION,
    download_mbps DOUBLE PRECISION,
    
    -- Data breakdown
    video_upload_bytes INTEGER DEFAULT 0,
    metadata_upload_bytes INTEGER DEFAULT 0,
    api_traffic_bytes INTEGER DEFAULT 0,
    
    FOREIGN KEY (interface_name) REFERENCES network_interfaces(interface_name) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_bandwidth_interface_time ON bandwidth_usage_log(interface_name, timestamp);

-- ============================================================================
-- SECTION 26: BACKUP AND RESTORE
-- ============================================================================

-- Backup configurations
CREATE TABLE IF NOT EXISTS backup_configurations (
    config_id SERIAL PRIMARY KEY,
    config_name TEXT NOT NULL UNIQUE,
    
    -- Backup target
    backup_type TEXT NOT NULL, -- CHM, CLOUD, EXTERNAL_DRIVE, NETWORK
    backup_destination TEXT NOT NULL, -- Path or URL
    
    -- What to backup
    include_recordings INTEGER DEFAULT 1,
    include_metadata INTEGER DEFAULT 1,
    include_events INTEGER DEFAULT 1,
    include_system_logs INTEGER DEFAULT 1,
    include_database INTEGER DEFAULT 1,
    
    -- Retention
    retention_days INTEGER DEFAULT 30,
    max_backup_size_gb INTEGER,
    
    -- Compression
    enable_compression INTEGER DEFAULT 1,
    compression_level INTEGER DEFAULT 6, -- 0-9
    
    -- Encryption
    enable_encryption INTEGER DEFAULT 1,
    encryption_algorithm TEXT DEFAULT 'AES-256',
    
    -- Schedule
    backup_schedule_id INTEGER,
    
    is_enabled INTEGER DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (backup_schedule_id) REFERENCES maintenance_schedules(schedule_id) ON DELETE SET NULL,
    
    CONSTRAINT chk_backup_type CHECK (backup_type IN ('CHM', 'CLOUD', 'EXTERNAL_DRIVE', 'NETWORK', 'FTP', 'SFTP')),
    CONSTRAINT chk_compression_level CHECK (compression_level BETWEEN 0 AND 9)
);

-- Backup execution history
CREATE TABLE IF NOT EXISTS backup_history (
    backup_id SERIAL PRIMARY KEY,
    config_id INTEGER NOT NULL,
    
    backup_start TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    backup_end TIMESTAMP,
    backup_duration_seconds INTEGER,
    
    -- Backup details
    backup_path TEXT,
    total_files INTEGER,
    total_size_bytes INTEGER,
    compressed_size_bytes INTEGER,
    compression_ratio DOUBLE PRECISION,
    
    -- Status
    status TEXT DEFAULT 'IN_PROGRESS',
    progress_percentage DOUBLE PRECISION DEFAULT 0.0,
    files_backed_up INTEGER DEFAULT 0,
    files_failed INTEGER DEFAULT 0,
    
    -- Verification
    is_verified INTEGER DEFAULT 0,
    verification_status TEXT,
    checksum TEXT,
    
    error_message TEXT,
    
    FOREIGN KEY (config_id) REFERENCES backup_configurations(config_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_backup_status CHECK (status IN ('IN_PROGRESS', 'COMPLETED', 'FAILED', 'PARTIAL', 'CANCELLED')),
    CONSTRAINT chk_verification_status CHECK (verification_status IN ('PENDING', 'PASSED', 'FAILED', NULL))
);

-- ============================================================================
-- SECTION 27: GEOFENCING AND LOCATION TRACKING
-- ============================================================================

-- Geofence definitions for route monitoring
CREATE TABLE IF NOT EXISTS geofences (
    geofence_id SERIAL PRIMARY KEY,
    geofence_name TEXT NOT NULL,
    geofence_type TEXT NOT NULL, -- STATION, DEPOT, RESTRICTED_AREA, ROUTE_SEGMENT
    
    -- Geometry (simplified polygon or circle)
    geometry_type TEXT NOT NULL, -- CIRCLE, POLYGON
    center_latitude DOUBLE PRECISION,
    center_longitude DOUBLE PRECISION,
    radius_meters DOUBLE PRECISION, -- For CIRCLE
    polygon_coordinates TEXT, -- JSON array for POLYGON
    
    -- Event triggers
    trigger_on_entry INTEGER DEFAULT 0,
    trigger_on_exit INTEGER DEFAULT 0,
    trigger_on_dwell INTEGER DEFAULT 0,
    dwell_threshold_seconds INTEGER,
    
    -- Metadata
    description TEXT,
    is_active INTEGER DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT chk_geofence_type CHECK (geofence_type IN ('STATION', 'DEPOT', 'RESTRICTED_AREA', 'ROUTE_SEGMENT', 'MAINTENANCE_ZONE')),
    CONSTRAINT chk_geometry_type CHECK (geometry_type IN ('CIRCLE', 'POLYGON', 'POLYLINE'))
);

-- Geofence events
CREATE TABLE IF NOT EXISTS geofence_events (
    geofence_event_id SERIAL PRIMARY KEY,
    geofence_id INTEGER NOT NULL,
    event_id INTEGER,
    
    event_type TEXT NOT NULL, -- ENTRY, EXIT, DWELL
    event_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- Location at event
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    speed_kmh DOUBLE PRECISION,
    
    -- Dwell time (for DWELL events)
    dwell_duration_seconds INTEGER,
    
    FOREIGN KEY (geofence_id) REFERENCES geofences(geofence_id) ON DELETE CASCADE,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_geofence_event_type CHECK (event_type IN ('ENTRY', 'EXIT', 'DWELL'))
);

-- ============================================================================
-- SECTION 28: QUALITY OF SERVICE (QoS) METRICS
-- ============================================================================

-- QoS metrics for video streams
CREATE TABLE IF NOT EXISTS stream_qos_metrics (
    metric_id SERIAL PRIMARY KEY,
    camera_id INTEGER NOT NULL,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    -- Streaming metrics
    bitrate_kbps INTEGER,
    frame_rate_fps DOUBLE PRECISION,
    resolution_width INTEGER,
    resolution_height INTEGER,
    
    -- Network quality
    packet_loss_percent DOUBLE PRECISION DEFAULT 0.0,
    jitter_ms DOUBLE PRECISION,
    latency_ms INTEGER,
    rtt_ms INTEGER, -- Round-trip time
    
    -- Error metrics
    dropped_frames INTEGER DEFAULT 0,
    corrupted_frames INTEGER DEFAULT 0,
    
    -- Buffer status
    buffer_level_percent DOUBLE PRECISION,
    buffer_underruns INTEGER DEFAULT 0,
    
    -- Overall quality score (0-100)
    quality_score DOUBLE PRECISION,
    quality_level TEXT, -- EXCELLENT, GOOD, FAIR, POOR, CRITICAL
    
    FOREIGN KEY (camera_id) REFERENCES cameras(camera_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_quality_level CHECK (quality_level IN ('EXCELLENT', 'GOOD', 'FAIR', 'POOR', 'CRITICAL'))
);

CREATE INDEX IF NOT EXISTS idx_qos_camera_time ON stream_qos_metrics(camera_id, timestamp);

-- ============================================================================
-- SECTION 29: EMERGENCY PROTOCOLS
-- ============================================================================

-- Emergency response procedures
CREATE TABLE IF NOT EXISTS emergency_procedures (
    procedure_id SERIAL PRIMARY KEY,
    procedure_name TEXT NOT NULL,
    trigger_event_type TEXT NOT NULL,
    
    -- Procedure definition
    priority_level INTEGER DEFAULT 1, -- 1=highest
    response_time_seconds INTEGER, -- Expected response time
    
    -- Actions (JSON array)
    automated_actions TEXT, -- e.g., ['RECORD_ALL', 'ALERT_CONTROL_CENTER', 'LOCK_CAMERAS']
    manual_actions TEXT, -- Instructions for operator
    
    -- Camera handling
    cameras_to_activate TEXT, -- JSON array of camera IDs or 'ALL'
    display_override INTEGER DEFAULT 0,
    
    -- Notifications
    notify_users TEXT, -- JSON array of user IDs or roles
    notification_message TEXT,
    
    is_active INTEGER DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Emergency activations log
CREATE TABLE IF NOT EXISTS emergency_activations (
    activation_id SERIAL PRIMARY KEY,
    procedure_id INTEGER NOT NULL,
    event_id INTEGER,
    
    activation_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    activated_by TEXT, -- AUTOMATIC or user_id
    
    -- Response tracking
    response_time_seconds INTEGER,
    responded_by TEXT,
    response_actions_taken TEXT, -- JSON array
    
    -- Status
    status TEXT DEFAULT 'ACTIVE', -- ACTIVE, RESPONDING, RESOLVED
    resolved_timestamp TIMESTAMP,
    resolution_notes TEXT,
    
    FOREIGN KEY (procedure_id) REFERENCES emergency_procedures(procedure_id) ON DELETE CASCADE,
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE,
    
    CONSTRAINT chk_emergency_status CHECK (status IN ('ACTIVE', 'RESPONDING', 'RESOLVED', 'FALSE_ALARM'))
);

-- ============================================================================
-- SECTION 30: FINAL UTILITY VIEWS
-- ============================================================================

-- View: System dashboard summary
CREATE OR REPLACE VIEW v_dashboard_summary AS
SELECT
    (SELECT COUNT(*) FROM cameras WHERE status = 'ACTIVE') as total_cameras,
    (SELECT COUNT(*) FROM cameras c 
     LEFT JOIN camera_health ch ON c.camera_id = ch.camera_id 
     WHERE ch.is_recording = 1 AND ch.health_id IN (
         SELECT MAX(health_id) FROM camera_health GROUP BY camera_id
     )) as recording_cameras,
    (SELECT COUNT(*) FROM recordings WHERE status = 'RECORDING') as active_recordings,
    (SELECT COUNT(*) FROM events WHERE status = 'ACTIVE') as active_events,
    (SELECT COUNT(*) FROM events WHERE status = 'ACTIVE' AND severity = 'CRITICAL') as critical_events,
    (SELECT SUM(used_capacity_gb) FROM storage_devices WHERE is_mounted = 1) as total_storage_used_gb,
    (SELECT SUM(total_capacity_gb) FROM storage_devices WHERE is_mounted = 1) as total_storage_capacity_gb,
    (SELECT ROUND(AVG(usage_percentage)::numeric, 2) FROM storage_devices WHERE is_mounted = 1) as avg_storage_usage_percent,
    (SELECT health_status FROM system_health ORDER BY timestamp DESC LIMIT 1) as system_health_status,
    (SELECT COUNT(*) FROM users WHERE is_active = 1) as active_users;

-- View: Camera status with sync info
CREATE OR REPLACE VIEW v_camera_sync_status AS
SELECT 
    c.camera_id,
    c.camera_name,
    c.camera_type,
    c.status,
    c.is_ptp_synced,
    c.is_ntp_synced,
    cm.mic_id,
    m.mic_name,
    cm.latency_offset_ms,
    cm.calibration_date,
    ch.is_online,
    ch.is_recording,
    ch.frame_rate_actual
FROM cameras c
LEFT JOIN camera_mic_mapping cm ON c.camera_id = cm.camera_id AND cm.is_active = 1
LEFT JOIN microphones m ON cm.mic_id = m.mic_id
LEFT JOIN camera_health ch ON c.camera_id = ch.camera_id
    AND ch.health_id = (
        SELECT health_id FROM camera_health 
        WHERE camera_id = c.camera_id 
        ORDER BY timestamp DESC LIMIT 1
    );

-- View: Recent critical events
CREATE OR REPLACE VIEW v_recent_critical_events AS
SELECT 
    e.event_id,
    e.event_type,
    e.severity,
    e.title,
    e.description,
    e.occurred_at,
    e.status,
    e.is_acknowledged,
    e.acknowledged_by,
    c.camera_name,
    e.gps_latitude,
    e.gps_longitude
FROM events e
LEFT JOIN cameras c ON e.camera_id = c.camera_id
WHERE e.severity IN ('CRITICAL', 'EMERGENCY')
    AND e.occurred_at >= NOW() - INTERVAL '24 hours'
ORDER BY e.occurred_at DESC;

-- ============================================================================
-- END OF SCHEMA
-- ============================================================================

-- Schema initialization complete
-- Total tables: 50+
-- Total views: 10+
-- Total triggers: 5+
-- Total indexes: 30+

-- PostgreSQL performance settings are configured in postgresql.conf
-- Recommended: shared_buffers=256MB, work_mem=4MB,
--              synchronous_commit=off (for embedded NVR use)

-- ============================================================================
-- POST-SCHEMA: Update cameras output paths for existing rows
-- ============================================================================
-- If you already have cameras in the table without output paths set,
-- run this UPDATE to populate them based on the system_config storage paths.
-- Adjust the base paths to match your actual storage_base and hls_base.

-- UPDATE cameras
-- SET
--     rec_output_dir   = '/storage/recordings/cam_' || camera_id::TEXT,
--     hls_output_dir   = '/storage/hls/cam_'        || camera_id::TEXT,
--     hls_playlist_url = '/hls/cam_'                || camera_id::TEXT || '/stream.m3u8'
-- WHERE rec_output_dir IS NULL;

COMMIT;

-- ============================================================================
-- POST-IMPORT VERIFICATION QUERIES
-- ============================================================================
-- Run these manually after import to verify success:
--
-- \echo '--- Tables created ---'
-- SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename;
--
-- \echo '--- Row counts for seeded tables ---'
-- SELECT 'system_config'       AS tbl, COUNT(*) FROM system_config
-- UNION ALL
-- SELECT 'recording_presets',          COUNT(*) FROM recording_presets
-- UNION ALL
-- SELECT 'maintenance_schedules',      COUNT(*) FROM maintenance_schedules
-- UNION ALL
-- SELECT 'users',                      COUNT(*) FROM users;
--
-- \echo '--- cameras table columns ---'
-- SELECT column_name, data_type, column_default
-- FROM information_schema.columns
-- WHERE table_name = 'cameras'
-- ORDER BY ordinal_position;
-- ============================================================================

-- ============================================================================
-- HEALTH MONITOR SETTINGS (single-row, managed by VMS)
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

    CONSTRAINT chk_poll_interval
        CHECK (poll_interval_sec > 0),
    CONSTRAINT chk_cpu_warn
        CHECK (cpu_warn_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_mem_warn
        CHECK (mem_warn_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_disk_warn
        CHECK (disk_warn_threshold BETWEEN 0 AND 100)
);

-- Seed a default row
INSERT INTO health_monitor_settings (poll_interval_sec) VALUES (10)
    ON CONFLICT DO NOTHING;

-- ============================================================================
-- RECORDING SETTINGS (single-row, managed by VMS)
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

    CONSTRAINT chk_recording_retention
        CHECK (recording_retention_days > 0),
    CONSTRAINT chk_segment_duration
        CHECK (segment_duration_sec > 0),
    CONSTRAINT chk_segment_size
        CHECK (segment_max_size_mb > 0),
    CONSTRAINT chk_max_storage
        CHECK (max_storage_gb > 0)
);

-- Seed a default row
INSERT INTO recording_settings (storage_path) VALUES ('/storage/recordings')
    ON CONFLICT DO NOTHING;

-- ============================================================================
-- HLS SETTINGS (single-row, managed by VMS)
-- ============================================================================
CREATE TABLE IF NOT EXISTS hls_settings (
    id                      SERIAL PRIMARY KEY,
    hls_base                TEXT     NOT NULL DEFAULT '/storage/hls',
    hls_segment_sec         INTEGER  NOT NULL DEFAULT 4,
    hls_window_size         INTEGER  NOT NULL DEFAULT 10,
    hls_delete_old_segments BOOLEAN  NOT NULL DEFAULT TRUE,
    updated_at              TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_hls_segment
        CHECK (hls_segment_sec > 0),
    CONSTRAINT chk_hls_window
        CHECK (hls_window_size > 0)
);

-- Seed a default row
INSERT INTO hls_settings (hls_base) VALUES ('/storage/hls')
    ON CONFLICT DO NOTHING;

-- ============================================================================
-- ONVIF GLOBAL SETTINGS (single-row, managed by VMS)
-- ============================================================================
CREATE TABLE IF NOT EXISTS onvif_settings (
    id                      SERIAL PRIMARY KEY,
    multicast_ip            VARCHAR(64)  NOT NULL DEFAULT '239.255.255.250',
    multicast_port          INTEGER      NOT NULL DEFAULT 3702,
    discovery_interval_sec  INTEGER      NOT NULL DEFAULT 60,
    probe_timeout_ms        INTEGER      NOT NULL DEFAULT 3000,
    enable_discovery        BOOLEAN      NOT NULL DEFAULT TRUE,
    updated_at              TIMESTAMP    NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_multicast_port
        CHECK (multicast_port BETWEEN 1 AND 65535),
    CONSTRAINT chk_discovery_interval
        CHECK (discovery_interval_sec > 0),
    CONSTRAINT chk_probe_timeout
        CHECK (probe_timeout_ms > 0)
);

-- Seed a default row so mNVR always finds settings on first boot
INSERT INTO onvif_settings (multicast_ip) VALUES ('239.255.255.250')
    ON CONFLICT DO NOTHING;

-- ============================================================================
-- CAMERA CONFIGURATION (per-camera ONVIF/RTSP credentials, managed by VMS)
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

    CONSTRAINT chk_onvif_port
        CHECK (onvif_port BETWEEN 1 AND 65535)
);

-- Seed a default row so mNVR always finds settings on first boot
-- INSERT INTO cameras_config_details
--   (camera_slot, name, location, camera_type, ip_address, onvif_port,
--    onvif_username, onvif_password, rtsp_username, rtsp_password)
-- VALUES
--   (1, 'Front Entrance', 'Front Entrance', 'DOOR', '172.210.140.91', 80,
--    'admin', 'bel123456', 'admin', 'bel123456') ON CONFLICT DO NOTHING;

-- Analyze all tables for query planner statistics
VACUUM ANALYZE;

-- ============================================================================
-- ONVIF PROFILE S EXTENSIONS
-- Adds: per-camera media profile cache (multi-stream), active-stream
-- selection, PTZ preset storage, imaging settings, and audio settings.
-- All statements are idempotent (IF NOT EXISTS / ON CONFLICT).
-- ============================================================================

-- ----------------------------------------------------------------------------
-- Cached ONVIF media profiles per camera (one row per profile/stream)
-- A camera typically exposes 2-3 profiles (e.g. "MainStream" 1080p,
-- "SubStream" 720p, "ThirdStream" CIF) — each independently configurable.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_media_profiles (
    id                  SERIAL PRIMARY KEY,
    camera_id           INTEGER NOT NULL REFERENCES cameras(camera_id) ON DELETE CASCADE,
    profile_token       VARCHAR(128) NOT NULL,   -- ONVIF ProfileToken
    profile_name        VARCHAR(128),
    encoding            VARCHAR(16),              -- H264 / H265 / JPEG
    resolution_width    INTEGER,
    resolution_height   INTEGER,
    fps                 INTEGER,
    bitrate_kbps        INTEGER,
    gov_length          INTEGER,                  -- GOP / keyframe interval
    quality             NUMERIC(4,1),
    stream_uri          TEXT,                      -- cached RTSP URI for this profile
    snapshot_uri        TEXT,
    is_active_stream    BOOLEAN NOT NULL DEFAULT FALSE,  -- which profile NVR/UI currently pulls
    video_source_token  VARCHAR(128),
    audio_encoder_token VARCHAR(128),
    last_synced_at      TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_camera_profile UNIQUE (camera_id, profile_token)
);

CREATE INDEX IF NOT EXISTS idx_camera_media_profiles_camera ON camera_media_profiles(camera_id);

-- ----------------------------------------------------------------------------
-- PTZ presets (per camera). Mirrors ONVIF PTZ preset tokens so "Goto Preset"
-- on the frontend maps 1:1 to the camera's own stored preset.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_ptz_presets (
    id              SERIAL PRIMARY KEY,
    camera_id       INTEGER NOT NULL REFERENCES cameras(camera_id) ON DELETE CASCADE,
    preset_token    VARCHAR(128) NOT NULL,     -- ONVIF PresetToken (may be camera-assigned)
    preset_name     VARCHAR(128) NOT NULL,
    pan             NUMERIC(8,4),
    tilt            NUMERIC(8,4),
    zoom            NUMERIC(8,4),
    is_home         BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMP NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_camera_preset_token UNIQUE (camera_id, preset_token)
);

CREATE INDEX IF NOT EXISTS idx_camera_ptz_presets_camera ON camera_ptz_presets(camera_id);

-- ----------------------------------------------------------------------------
-- PTZ current position / capability cache (single row per camera)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_ptz_status (
    camera_id       INTEGER PRIMARY KEY REFERENCES cameras(camera_id) ON DELETE CASCADE,
    pan             NUMERIC(8,4) DEFAULT 0,
    tilt            NUMERIC(8,4) DEFAULT 0,
    zoom            NUMERIC(8,4) DEFAULT 0,
    move_status     VARCHAR(16) DEFAULT 'IDLE',   -- IDLE / MOVING
    supports_continuous BOOLEAN DEFAULT TRUE,
    supports_relative   BOOLEAN DEFAULT TRUE,
    supports_absolute   BOOLEAN DEFAULT TRUE,
    min_pan NUMERIC(8,4) DEFAULT -1, max_pan  NUMERIC(8,4) DEFAULT 1,
    min_tilt NUMERIC(8,4) DEFAULT -1, max_tilt NUMERIC(8,4) DEFAULT 1,
    min_zoom NUMERIC(8,4) DEFAULT 0, max_zoom  NUMERIC(8,4) DEFAULT 1,
    updated_at      TIMESTAMP NOT NULL DEFAULT NOW()
);

-- ----------------------------------------------------------------------------
-- Imaging settings (brightness, contrast, saturation, sharpness, IR, etc.)
-- Single row per camera — mirrors ONVIF ImagingSettings20.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_image_settings (
    camera_id           INTEGER PRIMARY KEY REFERENCES cameras(camera_id) ON DELETE CASCADE,
    video_source_token  VARCHAR(128),
    brightness          NUMERIC(6,2) DEFAULT 50,
    contrast            NUMERIC(6,2) DEFAULT 50,
    color_saturation    NUMERIC(6,2) DEFAULT 50,
    sharpness           NUMERIC(6,2) DEFAULT 50,
    ir_cut_filter        VARCHAR(8) DEFAULT 'AUTO',   -- AUTO / ON / OFF
    wide_dynamic_range   BOOLEAN DEFAULT TRUE,
    backlight_compensation BOOLEAN DEFAULT FALSE,
    exposure_mode        VARCHAR(8) DEFAULT 'AUTO',   -- AUTO / MANUAL
    exposure_time_us     INTEGER,                      -- only used if MANUAL
    exposure_gain_db     NUMERIC(6,2),
    white_balance_mode   VARCHAR(8) DEFAULT 'AUTO',    -- AUTO / MANUAL
    focus_mode           VARCHAR(8) DEFAULT 'AUTO',    -- AUTO / MANUAL
    -- Capability ranges reported by the camera (for UI slider bounds)
    brightness_min NUMERIC(6,2) DEFAULT 0,  brightness_max NUMERIC(6,2) DEFAULT 100,
    contrast_min   NUMERIC(6,2) DEFAULT 0,  contrast_max   NUMERIC(6,2) DEFAULT 100,
    saturation_min NUMERIC(6,2) DEFAULT 0,  saturation_max NUMERIC(6,2) DEFAULT 100,
    sharpness_min  NUMERIC(6,2) DEFAULT 0,  sharpness_max  NUMERIC(6,2) DEFAULT 100,
    last_synced_at      TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMP NOT NULL DEFAULT NOW()
);

-- ----------------------------------------------------------------------------
-- Audio settings (per camera audio encoder / source configuration)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_audio_settings (
    camera_id            INTEGER PRIMARY KEY REFERENCES cameras(camera_id) ON DELETE CASCADE,
    audio_source_token   VARCHAR(128),
    audio_encoder_token  VARCHAR(128),
    encoding             VARCHAR(16) DEFAULT 'AAC',   -- G711 / G726 / AAC
    bitrate_kbps         INTEGER DEFAULT 64,
    sample_rate_khz      INTEGER DEFAULT 16,
    input_gain_db        NUMERIC(6,2) DEFAULT 0,
    is_enabled           BOOLEAN DEFAULT TRUE,
    last_synced_at       TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at           TIMESTAMP NOT NULL DEFAULT NOW()
);

-- ----------------------------------------------------------------------------
-- ONVIF capability snapshot per camera (what services/features a camera
-- actually advertises — drives which UI controls are shown/hidden)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS camera_onvif_capabilities (
    camera_id          INTEGER PRIMARY KEY REFERENCES cameras(camera_id) ON DELETE CASCADE,
    supports_media     BOOLEAN DEFAULT TRUE,
    supports_ptz       BOOLEAN DEFAULT FALSE,
    supports_imaging   BOOLEAN DEFAULT FALSE,
    supports_audio     BOOLEAN DEFAULT FALSE,
    supports_events    BOOLEAN DEFAULT FALSE,
    profile_s_conformant BOOLEAN DEFAULT FALSE,
    device_service_url TEXT,
    media_service_url  TEXT,
    ptz_service_url    TEXT,
    imaging_service_url TEXT,
    last_checked_at    TIMESTAMP NOT NULL DEFAULT NOW()
);

VACUUM ANALYZE;
