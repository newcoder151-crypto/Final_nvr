# ===========================================================================
# mNVR System - Makefile
# ===========================================================================
# Required dependencies (Ubuntu/Debian):
#   sudo apt install \
#     libgstreamer1.0-dev \
#     libgstreamer-plugins-base1.0-dev \
#     gstreamer1.0-plugins-good \
#     gstreamer1.0-plugins-bad \
#     gstreamer1.0-libav \
#     libpq-dev
#
# Optional (enables REST API):
#   sudo apt install libmicrohttpd-dev
#
# Build:   make              (release)
#          make debug        (debug + AddressSanitizer)
#          make clean
#          make install      (to /opt/mnvr)
#          make check        (syntax check only)
# ===========================================================================

CC      := gcc
SRCDIR  := src
OUTDIR  := build
TARGET  := $(OUTDIR)/mnvrd

# ---------------------------------------------------------------------------
# Auto-detect optional dependencies
# ---------------------------------------------------------------------------
HAS_MHD := $(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo yes || echo no)

ifeq ($(HAS_MHD),yes)
  MHD_CFLAGS := -DMNVR_WITH_API $(shell pkg-config --cflags libmicrohttpd 2>/dev/null)
  MHD_LIBS   := $(shell pkg-config --libs   libmicrohttpd 2>/dev/null)
  $(info REST API: ENABLED  (libmicrohttpd found))
else
  MHD_CFLAGS :=
  MHD_LIBS   :=
  $(info REST API: DISABLED (libmicrohttpd not found - install libmicrohttpd-dev to enable))
endif

# ---------------------------------------------------------------------------
# pkg-config flags for required deps
# ---------------------------------------------------------------------------
GST_CFLAGS  := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS    := $(shell pkg-config --libs   gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
# RTSP relay server (re-exposes the watermarked restream to MediaMTX)
RTSPSRV_CFLAGS := $(shell pkg-config --cflags gstreamer-rtsp-server-1.0 2>/dev/null)
RTSPSRV_LIBS   := $(shell pkg-config --libs   gstreamer-rtsp-server-1.0 2>/dev/null)
# PostgreSQL client library (required)
PG_CFLAGS   := $(shell pkg-config --cflags libpq 2>/dev/null)
PG_LIBS     := $(shell pkg-config --libs   libpq 2>/dev/null || echo -lpq)
MATH_LIBS   := -lm

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
INCLUDES := \
    -I$(SRCDIR)/include \
    -I$(SRCDIR) \
    -I$(SRCDIR)/modules/logger \
    -I$(SRCDIR)/modules/config \
    -I$(SRCDIR)/modules/onvif \
    -I$(SRCDIR)/modules/recorder \
    -I$(SRCDIR)/modules/recorder/original_gstreamer_module \
    -I$(SRCDIR)/modules/hls \
    -I$(SRCDIR)/modules/streamer \
    -I$(SRCDIR)/modules/ai \
    -I$(SRCDIR)/modules/health \
    -I$(SRCDIR)/modules/api \
    -I$(SRCDIR)/modules/rtsp_relay \
    -I$(SRCDIR)/modules/gps \
    -I$(SRCDIR)/db \
    $(GST_CFLAGS) \
    $(RTSPSRV_CFLAGS) \
    $(PG_CFLAGS) \
    $(MHD_CFLAGS)

CFLAGS  := -Wall -Wextra -std=c11 -O2 $(INCLUDES)
LDFLAGS := -lpthread $(GST_LIBS) $(RTSPSRV_LIBS) $(PG_LIBS) $(MHD_LIBS) $(MATH_LIBS)

# ---------------------------------------------------------------------------
# mNVR module source files
# ---------------------------------------------------------------------------
MNVR_SRCS := \
    $(SRCDIR)/core/main.c \
    $(SRCDIR)/modules/logger/logger.c \
    $(SRCDIR)/modules/config/config_module.c \
    $(SRCDIR)/modules/onvif/onvif_module.c \
    $(SRCDIR)/modules/recorder/recorder_module.c \
    $(SRCDIR)/modules/hls/hls_module.c \
    $(SRCDIR)/modules/streamer/streamer_module.c \
    $(SRCDIR)/modules/ai/ai_module.c \
    $(SRCDIR)/modules/health/health_module.c \
    $(SRCDIR)/modules/api/api_module.c \
    $(SRCDIR)/modules/rtsp_relay/rtsp_relay.c \
    $(SRCDIR)/modules/gps/gps_module.c \
    $(SRCDIR)/db/db_module.c

# ---------------------------------------------------------------------------
# Original GStreamer recorder module (compiled unchanged)
# recorder_module.c is the adapter/wrapper for these files.
# Note: original config.c included for config_init_defaults() symbol.
# ---------------------------------------------------------------------------
ORIG_DIR  := $(SRCDIR)/modules/recorder/original_gstreamer_module
ORIG_SRCS := \
    $(ORIG_DIR)/recorder.c \
    $(ORIG_DIR)/manager.c \
    $(ORIG_DIR)/callbacks.c \
    $(ORIG_DIR)/utils.c \
    $(ORIG_DIR)/config.c

ALL_SRCS := $(MNVR_SRCS) $(ORIG_SRCS)

# ---------------------------------------------------------------------------
# Object file derivation
# ---------------------------------------------------------------------------
MNVR_OBJS := $(patsubst $(SRCDIR)/%.c, $(OUTDIR)/%.o, $(MNVR_SRCS))
ORIG_OBJS := $(patsubst $(ORIG_DIR)/%.c, $(OUTDIR)/orig/%.o, $(ORIG_SRCS))
ALL_OBJS  := $(MNVR_OBJS) $(ORIG_OBJS)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all debug clean install check

all: $(TARGET)

debug: CFLAGS  += -g -O0 -DDEBUG -fsanitize=address
debug: LDFLAGS += -fsanitize=address
debug: $(TARGET)

$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  Built: $@  ($(shell du -sh $@ 2>/dev/null | cut -f1))"
ifeq ($(HAS_MHD),yes)
	@echo "  REST API: enabled on port 8443 (set api_port in mnvr.conf)"
else
	@echo "  REST API: disabled (install libmicrohttpd-dev to enable)"
endif
	@echo ""

# mNVR module objects
$(OUTDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Original GStreamer module objects (relaxed warnings - unmodified upstream code)
$(OUTDIR)/orig/%.o: $(ORIG_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) \
	    -Wno-unused-parameter \
	    -Wno-unused-result \
	    -Wno-implicit-function-declaration \
	    -D_POSIX_C_SOURCE=200809L \
	    -c $< -o $@

clean:
	rm -rf $(OUTDIR)

install: all
	install -d /opt/mnvr/bin
	install -m 755 $(TARGET) /opt/mnvr/bin/mnvrd
	install -d /etc/mnvr
	[ -f /etc/mnvr/mnvr.conf ] || \
	    install -m 644 config/mnvr.conf.example /etc/mnvr/mnvr.conf
	install -m 644 database.sch /etc/mnvr/database.sch
	install -d /var/log/mnvr /storage/recordings /storage/hls
	install -m 644 deploy/mnvrd.service /etc/systemd/system/mnvrd.service
	systemctl daemon-reload || true
	@echo ""
	@echo "  Installed to /opt/mnvr"
	@echo "  Run: systemctl enable --now mnvrd"
	@echo ""

# Syntax-only check (fast, no link)
check:
	@echo "Checking mNVR modules..."
	@FAIL=0; for f in $(MNVR_SRCS); do \
	    OUT=$$($(CC) $(CFLAGS) -fsyntax-only $$f 2>&1); \
	    E=$$(echo "$$OUT" | grep -c ": error:" || true); \
	    W=$$(echo "$$OUT" | grep -c ": warning:" || true); \
	    printf "  %-52s E=%-2s W=%s\n" $$f $$E $$W; \
	    [ "$$E" -gt 0 ] && FAIL=1 && echo "$$OUT" | grep ": error:" || true; \
	done; \
	[ "$$FAIL" -eq 0 ] && echo "  All OK" || (echo "  FAILED" && exit 1)
