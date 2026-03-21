CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
          $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
          $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale)
LDFLAGS = $(shell pkg-config --libs sdl2 SDL2_image SDL2_ttf) \
          $(shell pkg-config --libs libavformat libavcodec libavutil libswresample libswscale) \
          -lm

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.c \
          $(SRC_DIR)/platform.c \
          $(SRC_DIR)/filebrowser.c \
          $(SRC_DIR)/browser.c \
          $(SRC_DIR)/history.c \
          $(SRC_DIR)/hintbar.c \
          $(SRC_DIR)/overlay.c \
          $(SRC_DIR)/theme.c \
          $(SRC_DIR)/decoder.c \
          $(SRC_DIR)/audio.c \
          $(SRC_DIR)/video.c \
          $(SRC_DIR)/player.c \
          $(SRC_DIR)/resume.c
TARGET  = gvu

# -------------------------------------------------------------------------
# Miyoo A30 cross-compile settings
# Intended to run INSIDE the Docker container built from Dockerfile.gvu.
# pkg-config env vars are set by the Dockerfile (PKG_CONFIG_PATH etc.).
# -------------------------------------------------------------------------
A30_CC      = arm-linux-gnueabihf-gcc
A30_CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L \
              -DGVU_A30 \
              -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
              $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf \
                  libavformat libavcodec libavutil libswresample libswscale)
# Static linking: all SDL2+FFmpeg libs baked into the binary.
# gvu32 NEEDED at runtime: only libc, libm, libpthread, libdl, libasound.so.2
# (SDL2 dlopen's libasound at runtime — A30 has it natively).
A30_LDFLAGS = -Wl,--start-group \
              $(shell pkg-config --static --libs sdl2 SDL2_image SDL2_ttf \
                  libavformat libavcodec libavutil libswresample libswscale) \
              -Wl,--end-group \
              -lm -lpthread -ldl -static-libgcc -static-libstdc++

A30_SRCS    = $(SRCS) $(SRC_DIR)/a30_screen.c $(SRC_DIR)/glibc_compat.c
A30_TARGET  = gvu32

GVU_A30_IMAGE  ?= gvu-a30
GVU_A30_DEPLOY ?= spruce@192.168.1.62
GVU_A30_PATH   ?= /mnt/SDCARD/App/GVU

.PHONY: all clean test miyoo-a30-build miyoo-a30-docker miyoo-a30-package miyoo-a30-deploy

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Build with local test media paths (/tmp/gvu_test/) instead of /mnt/SDCARD/
test: $(SRCS)
	$(CC) $(CFLAGS) -DGVU_TEST_ROOTS -o $(TARGET) $^ $(LDFLAGS)

# ---- A30 targets ---------------------------------------------------------

# Cross-compile inside Docker (called by miyoo-a30-docker, or directly inside container)
miyoo-a30-build: $(A30_SRCS)
	$(A30_CC) $(A30_CFLAGS) -o $(A30_TARGET) $^ $(A30_LDFLAGS)
	@echo "Built: $(A30_TARGET)"

# Build Docker image + compile gvu32 + collect libs
miyoo-a30-docker:
	docker build -f cross-compile/miyoo-a30/Dockerfile.gvu \
	             -t $(GVU_A30_IMAGE) .
	mkdir -p build
	docker run --rm -v $(CURDIR):/gvu $(GVU_A30_IMAGE) \
	           sh cross-compile/miyoo-a30/build_inside_docker.sh

# Assemble SpruceOS zip from build/ artifacts
miyoo-a30-package: VERSION ?= test
miyoo-a30-package:
	sh cross-compile/miyoo-a30/package_gvu_a30.sh $(VERSION)

# Deploy directly to device over SSH (no zip — raw install for testing)
miyoo-a30-deploy: build/gvu32
	ssh $(GVU_A30_DEPLOY) "mkdir -p $(GVU_A30_PATH)/libs32 $(GVU_A30_PATH)/resources/fonts"
	scp build/gvu32 $(GVU_A30_DEPLOY):$(GVU_A30_PATH)/
	scp -r build/libs32 $(GVU_A30_DEPLOY):$(GVU_A30_PATH)/
	scp cross-compile/miyoo-a30/gvu_base/launch.sh \
	    cross-compile/miyoo-a30/gvu_base/config.json \
	    $(GVU_A30_DEPLOY):$(GVU_A30_PATH)/
	scp resources/fonts/DejaVuSans.ttf \
	    $(GVU_A30_DEPLOY):$(GVU_A30_PATH)/resources/fonts/
	scp resources/default_cover.png \
	    resources/scrape_covers.sh \
	    $(GVU_A30_DEPLOY):$(GVU_A30_PATH)/resources/
	ssh $(GVU_A30_DEPLOY) "chmod +x $(GVU_A30_PATH)/launch.sh $(GVU_A30_PATH)/gvu32 $(GVU_A30_PATH)/resources/scrape_covers.sh"
	@echo "Deployed to $(GVU_A30_DEPLOY):$(GVU_A30_PATH)"

clean:
	rm -f $(TARGET) $(A30_TARGET)
	rm -rf build/gvu32 build/libs32
