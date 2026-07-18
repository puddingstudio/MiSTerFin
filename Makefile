SRCS = src/main.c src/fb.c src/ddr.c src/jellyfin.c

TARGET     = misterfin
TARGET_ARM = misterfin-arm

VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo "dev")

CC     = gcc
CFLAGS = -O2 -Wall -Wextra -Isrc -DAPP_VERSION=\"$(VERSION)\"

CC_ARM     = zig cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a9
CFLAGS_ARM = -O2 -Isrc -DAPP_VERSION=\"$(VERSION)\" -D_FILE_OFFSET_BITS=64

MISTER_HOST ?= 192.168.2.225

.PHONY: all arm clean deploy

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

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
