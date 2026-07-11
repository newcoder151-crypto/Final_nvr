# Makefile for Professional Multi-Camera NVR Recorder
# Modular build system with separate compilation units

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
GSTREAMER_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0)
GSTREAMER_LIBS = $(shell pkg-config --libs gstreamer-1.0)

# Target executable
TARGET = nvr_recorder

# Source files
SOURCES = main.c \
          config.c \
          recorder.c \
          manager.c \
          callbacks.c \
          utils.c

# Object files (generated from source files)
OBJECTS = $(SOURCES:.c=.o)

# Header files (for dependency tracking)
HEADERS = config.h \
          recorder.h \
          manager.h \
          callbacks.h \
          utils.h

# Default target
all: $(TARGET)

# Link object files to create executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(GSTREAMER_CFLAGS) -o $(TARGET) $(OBJECTS) $(GSTREAMER_LIBS)
	@echo "Build complete: $(TARGET)"

# Compile source files to object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $(GSTREAMER_CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(TARGET) $(OBJECTS)
	@echo "Clean complete"

# Install to system directory
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

# Uninstall from system directory
uninstall:
	rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstalled from /usr/local/bin/$(TARGET)"

# Generate documentation using Doxygen (if available)
docs:
	@if command -v doxygen >/dev/null 2>&1; then \
		echo "Generating documentation..."; \
		doxygen Doxyfile 2>/dev/null || echo "Note: Create Doxyfile for documentation generation"; \
	else \
		echo "Doxygen not found. Install doxygen to generate documentation."; \
	fi

# Show help
help:
	@echo "Professional Multi-Camera NVR Recorder - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the executable (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  install    - Install to /usr/local/bin/"
	@echo "  uninstall  - Remove from /usr/local/bin/"
	@echo "  docs       - Generate Doxygen documentation"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Modules:"
	@echo "  main.c       - Program entry point"
	@echo "  config.c     - Configuration file parsing"
	@echo "  recorder.c   - Individual camera recording"
	@echo "  manager.c    - Multi-camera management"
	@echo "  callbacks.c  - GStreamer callbacks"
	@echo "  utils.c      - Utility functions"

# Phony targets (not files)
.PHONY: all clean install uninstall docs help
