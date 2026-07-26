SRCS = src/main.c src/fb.c src/ddr.c src/jellyfin.c src/json.c src/subtitles.c

TARGET     = misterfin
TARGET_ARM = misterfin-arm

VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo "dev")

CC     = gcc
CFLAGS = -O2 -Wall -Wextra -Isrc -DAPP_VERSION=\"$(VERSION)\"

CC_ARM     = zig cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a9
CFLAGS_ARM = -O2 -Isrc -DAPP_VERSION=\"$(VERSION)\" -D_FILE_OFFSET_BITS=64

MISTER_HOST ?= mister.local

.PHONY: all arm clean deploy test

all: $(TARGET)

# Unit tests. Host build only; neither of these has anything platform-specific
# in it. The JSON parser sits under every server response the client reads;
# the subtitle clean-up can only otherwise be checked during playback, which
# needs real hardware.
test:
	$(CC) $(CFLAGS) -o /tmp/misterfin_test_json tests/test_json.c src/json.c
	@/tmp/misterfin_test_json
	$(CC) $(CFLAGS) -o /tmp/misterfin_test_subtitles \
		tests/test_subtitles.c src/subtitles.c src/jellyfin.c src/json.c
	@/tmp/misterfin_test_subtitles
	$(CC) $(CFLAGS) -o /tmp/misterfin_test_config \
		tests/test_config.c src/jellyfin.c src/json.c
	@mkdir -p /tmp/misterfin_test_cfgdir && cd /tmp/misterfin_test_cfgdir && /tmp/misterfin_test_config

# -lm: stb_image needs pow(), the Toasty sprite paths need sinf/sincosf. The
# ARM build gets libm folded into libc by zig's target libc, so only the host
# build has to ask for it explicitly.
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

arm: $(SRCS)
	$(CC_ARM) $(CFLAGS_ARM) -o $(TARGET_ARM) $^ -lpthread -ldl

clean:
	rm -f $(TARGET) $(TARGET_ARM)

# Requires mplayer-arm (the fbdev-patched build from MiSTerDVD's docker/ toolchain)
# to already exist in this directory — see README "Building from Source".
deploy: arm
	sshpass -p "1" ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no root@$(MISTER_HOST) \
		"mkdir -p /media/fat/misterfin"
	sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
		$(TARGET_ARM) root@$(MISTER_HOST):/media/fat/misterfin/misterfin-arm
	@if [ -d assets/font ]; then \
		sshpass -p "1" ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no root@$(MISTER_HOST) \
			"mkdir -p /media/fat/misterfin/font"; \
		sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
			assets/font/font.desc assets/font/font-alpha.raw assets/font/font-bitmap.raw \
			root@$(MISTER_HOST):/media/fat/misterfin/font/; \
		echo "font deployed"; \
	fi
	@if [ -d assets/subfont ]; then \
		sshpass -p "1" ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no root@$(MISTER_HOST) \
			"mkdir -p /media/fat/misterfin/subfont"; \
		sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
			assets/subfont/font.desc assets/subfont/font-alpha.raw assets/subfont/font-bitmap.raw \
			root@$(MISTER_HOST):/media/fat/misterfin/subfont/; \
		echo "subfont deployed"; \
	fi
	@if [ -d assets/toasty ]; then \
		sshpass -p "1" ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no root@$(MISTER_HOST) \
			"mkdir -p /media/fat/misterfin/toasty"; \
		sshpass -p "1" scp -r -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
			assets/toasty/. root@$(MISTER_HOST):/media/fat/misterfin/toasty/; \
		echo "toasty sprites deployed"; \
	fi
	@if [ -f assets/about.png ]; then \
		sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
			assets/about.png root@$(MISTER_HOST):/media/fat/misterfin/about.png; \
		echo "about.png deployed"; \
	fi
	@if [ -f mplayer-arm ]; then \
		sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
			mplayer-arm root@$(MISTER_HOST):/media/fat/misterfin/mplayer-arm; \
		echo "mplayer-arm deployed"; \
	else \
		echo "WARNING: mplayer-arm not found in project root — copy it from a MiSTerDVD build before deploying"; \
	fi
	sshpass -p "1" scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no \
		tools/MiSTerFin.sh root@$(MISTER_HOST):/media/fat/Scripts/MiSTerFin.sh
	sshpass -p "1" ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no root@$(MISTER_HOST) \
		"chmod +x /media/fat/Scripts/MiSTerFin.sh"
	@echo "Deployed. Run MiSTerFin from the Scripts menu."
	@echo "Don't forget to create /media/fat/misterfin/jellyfin.conf (see jellyfin.conf.example)."
