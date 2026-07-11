-- Docker seed — runs once on first postgres container start
-- Admin password: Admin@123 (bcrypt hash)
INSERT INTO users (username, password_hash, full_name, email, role, is_active)
VALUES ('admin',
  '$2a$12$LQv3c1yqBWVHxkd0LHAkCOYz6TtxMQyCAgH8.8VnH8I0D2tT6W1Fi',
  'System Administrator', 'admin@railway.gov.in', 'ADMIN', 1)
ON CONFLICT (username) DO UPDATE SET password_hash=EXCLUDED.password_hash, is_active=1;

INSERT INTO users (username, password_hash, full_name, email, role, is_active)
VALUES ('operator',
  '$2a$12$LQv3c1yqBWVHxkd0LHAkCOYz6TtxMQyCAgH8.8VnH8I0D2tT6W1Fi',
  'NVR Operator', 'operator@railway.gov.in', 'OPERATOR', 1)
ON CONFLICT (username) DO NOTHING;

-- Cameras with real credentials (username=admin, password=bel123456)
INSERT INTO cameras (camera_name, camera_type, ip_address, rtsp_url, rtsp_port,
  username, password_hash, location_description, target_fps, status)
VALUES
  ('CAM-01-INTERIOR','INTERIOR','192.168.1.100','rtsp://192.168.1.100:554/stream1',554,
   'admin','bel123456','Coach S1 — Main Camera (Real IP)',25,'ACTIVE'),
  ('CAM-02-INTERIOR','INTERIOR','192.168.1.101','rtsp://192.168.1.101:554/stream1',554,
   'admin','bel123456','Coach S1 — Front Passenger Area',25,'ACTIVE'),
  ('CAM-03-INTERIOR','INTERIOR','192.168.1.102','rtsp://192.168.1.102:554/stream1',554,
   'admin','bel123456','Coach S1 — Rear Passenger Area',25,'ACTIVE'),
  ('CAM-04-DOOR','DOOR','192.168.1.103','rtsp://192.168.1.103:554/stream1',554,
   'admin','bel123456','Door 1 — Left Entry',25,'ACTIVE'),
  ('CAM-05-EXTERIOR','EXTERIOR','192.168.1.104','rtsp://192.168.1.104:554/stream1',554,
   'admin','bel123456','Exterior — Front View',25,'ACTIVE'),
  ('CAM-06-DRIVER','DRIVER_CAB','192.168.1.105','rtsp://192.168.1.105:554/stream1',554,
   'admin','bel123456','Driver Cab',25,'ACTIVE')
ON CONFLICT DO NOTHING;

-- Seed camera health
INSERT INTO camera_health (camera_id, is_online, is_recording, frame_rate_actual, bitrate_kbps, error_count)
SELECT camera_id, 0, 0, 0, 0, 0 FROM cameras ON CONFLICT DO NOTHING;

-- Sample events
DO $$
DECLARE
  cid1 INTEGER; cid4 INTEGER; cid6 INTEGER;
BEGIN
  SELECT camera_id INTO cid1 FROM cameras WHERE camera_name='CAM-01-INTERIOR' LIMIT 1;
  SELECT camera_id INTO cid4 FROM cameras WHERE camera_name='CAM-04-DOOR' LIMIT 1;
  SELECT camera_id INTO cid6 FROM cameras WHERE camera_name='CAM-06-DRIVER' LIMIT 1;
  IF cid4 IS NOT NULL THEN
    INSERT INTO events(event_type,title,severity,camera_id,description,occurred_at)
    VALUES('CROWD_DENSITY','High crowd density near door','WARNING',cid4,
           '7 persons detected, density 74%', NOW()-INTERVAL '2 hours') ON CONFLICT DO NOTHING;
  END IF;
  IF cid1 IS NOT NULL THEN
    INSERT INTO events(event_type,title,severity,camera_id,description,occurred_at)
    VALUES('INTRUSION','Unauthorized access detected','CRITICAL',cid1,
           'Person in restricted zone', NOW()-INTERVAL '45 minutes') ON CONFLICT DO NOTHING;
  END IF;
  IF cid6 IS NOT NULL THEN
    INSERT INTO events(event_type,title,severity,camera_id,description,occurred_at)
    VALUES('PHONE_USE','Mobile phone use by crew','WARNING',cid6,
           'Cell phone detected in driver cab', NOW()-INTERVAL '15 minutes') ON CONFLICT DO NOTHING;
  END IF;
END $$;
