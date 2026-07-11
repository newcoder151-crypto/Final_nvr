# Professional Multi-Camera NVR Recorder

A modular, professional-grade Network Video Recorder (NVR) for capturing multiple RTSP camera streams simultaneously using GStreamer.

## Features

- **Multi-camera support**: Record up to 16 cameras simultaneously
- **File segmentation**: Automatic splitting by size or duration
- **RTSP protocol**: Industry-standard IP camera support
- **MP4 output**: Fragmented MP4 for streamability
- **Configurable**: INI-style configuration file
- **Thread-safe**: Each camera runs in its own thread
- **Graceful shutdown**: Proper file closure on Ctrl+C

## Project Structure

### Modules

| Module | Files | Purpose |
|--------|-------|---------|
| **Main** | `main.c` | Program entry point and lifecycle management |
| **Configuration** | `config.c`, `config.h` | Configuration file parsing and management |
| **Recorder** | `recorder.c`, `recorder.h` | Individual camera recording logic |
| **Manager** | `manager.c`, `manager.h` | Multi-camera coordination |
| **Callbacks** | `callbacks.c`, `callbacks.h` | GStreamer event handlers |
| **Utilities** | `utils.c`, `utils.h` | Helper functions and signal handling |

### Module Responsibilities

#### main.c
- GStreamer initialization
- Command-line argument parsing
- Signal handler registration
- Manager lifecycle orchestration

#### config.c/h
- INI-style configuration parsing
- Default value initialization
- Global and per-camera settings
- Output directory creation

#### recorder.c/h
- Camera recorder creation and destruction
- GStreamer pipeline construction
- Recording thread management
- Pipeline start/stop control

#### manager.c/h
- Camera array management
- Thread-safe camera addition
- Bulk start/stop operations
- Resource cleanup coordination

#### callbacks.c/h
- RTSP pad linking (`on_pad_added`)
- Pipeline bus messages (`bus_call`)
- Segment filename generation (`format_location_callback`)

#### utils.c/h
- Directory creation utilities
- Signal handler implementation
- Global manager access

## Building

### Prerequisites

```bash
# RHEL/CentOS/AlmaLinux
sudo dnf install gcc gstreamer1-devel gstreamer1-plugins-base-devel

# Ubuntu/Debian
sudo apt install build-essential libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```

### Compile

```bash
make
```

### Install (optional)

```bash
sudo make install
```

### Clean

```bash
make clean
```

## Configuration File Format

Create a `.conf` file with the following structure:

```ini
# Global defaults (optional)
[global]
enable_segmentation=true
max_file_duration_sec=3600
max_file_size_mb=2048
rtsp_latency_ms=200
rtsp_timeout_sec=10
use_tcp=true
file_extension=mp4
add_timestamp=true

# Camera definitions
[camera_1]
name=Front Door
camera_url=rtsp://admin:password@192.168.1.100:554/stream1
output_file=/recordings/front_door

[camera_2]
name=Back Yard
camera_url=rtsp://admin:password@192.168.1.101:554/stream1
output_file=/recordings/back_yard
max_file_duration_sec=1800  # Override global setting
```

### Configuration Parameters

#### Global Section
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enable_segmentation` | boolean | false | Enable automatic file segmentation |
| `max_file_duration_sec` | integer | 3600 | Maximum segment duration in seconds |
| `max_file_size_mb` | integer | 0 | Maximum segment size in MB (0=unlimited) |
| `rtsp_latency_ms` | integer | 200 | RTSP stream latency in milliseconds |
| `rtsp_timeout_sec` | integer | 10 | RTSP connection timeout in seconds |
| `use_tcp` | boolean | true | Use TCP transport (false=UDP) |
| `file_extension` | string | mp4 | Output file extension |
| `add_timestamp` | boolean | true | Add timestamp to filenames |

#### Camera Section
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | no | Camera display name |
| `camera_url` | string | **yes** | RTSP stream URL |
| `output_file` | string | **yes** | Output file path prefix |

Camera sections can override any global parameter.

## Usage

```bash
./nvr_recorder cameras.conf
```

### Example Session

```
$ ./nvr_recorder cameras.conf
Loaded 2 cameras from config file
Added Camera 0 (Front Door)
Added Camera 1 (Back Yard)

=== Starting 2 cameras ===

[Front Door] Starting recording
  URL: rtsp://admin:password@192.168.1.100:554/stream1
  Output: /recordings/front_door
  Segmentation: 2048 MB 3600 sec 
[Back Yard] Starting recording
  URL: rtsp://admin:password@192.168.1.101:554/stream1
  Output: /recordings/back_yard

=== All cameras started ===

Recording 2 cameras... Press Ctrl+C to stop

[Front Door] State: NULL -> READY
[Front Door] State: READY -> PAUSED
[Back Yard] State: NULL -> READY
[Front Door] Starting segment 0: /recordings/front_door_20250119_143022_seg00000.mp4
[Front Door] State: PAUSED -> PLAYING
[Back Yard] State: READY -> PAUSED
[Back Yard] Starting segment 0: /recordings/back_yard_20250119_143022_seg00000.mp4
[Back Yard] State: PAUSED -> PLAYING

^C
Received signal 2, stopping all recordings...

=== Stopping 2 cameras ===

[Front Door] Stopping...
[Front Door] Stopped
[Back Yard] Stopping...
[Back Yard] Stopped

=== All cameras stopped ===

All recordings completed
```

## Output Files

### With Timestamp (default)
```
/recordings/front_door_20250119_143022_seg00000.mp4
/recordings/front_door_20250119_153022_seg00001.mp4
```

### Without Timestamp
```
/recordings/front_door_seg00000.mp4
/recordings/front_door_seg00001.mp4
```

### Continuous (no segmentation)
```
/recordings/front_door.mp4
```

## GStreamer Pipeline Topology

### Segmented Recording
```
rtspsrc -> rtph264depay -> h264parse -> queue -> splitmuxsink
                                                  (contains mp4mux)
```

### Continuous Recording
```
rtspsrc -> rtph264depay -> h264parse -> mp4mux -> filesink
```

## Documentation

All code is documented using Doxygen-style comments. To generate HTML documentation:

```bash
# Create basic Doxyfile
doxygen -g

# Generate documentation
make docs
```

## Troubleshooting

### Connection Issues
- Verify RTSP URL is correct
- Check network connectivity to camera
- Ensure camera credentials are valid
- Try changing `use_tcp` to false if TCP fails

### Pipeline Errors
- Ensure GStreamer plugins are installed
- Check `gst-inspect-1.0 splitmuxsink` works
- Verify output directory exists and is writable

### Segmentation Issues
- Ensure sufficient disk space
- Check file permissions in output directory
- Verify size/duration limits are reasonable

## License

This software is provided as-is for professional NVR recording applications.

## Author

Developed for professional multi-camera NVR deployments using GStreamer.
