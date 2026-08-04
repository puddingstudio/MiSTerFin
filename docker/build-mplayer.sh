#!/bin/bash
set -e

PREFIX=/build/sysroot
mkdir -p $PREFIX

# pkg-config must search our sysroot
export PKG_CONFIG=pkg-config
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig

MPLAYER_VER=1.5

CFLAGS_ARM="-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2"

# ── MPlayer ──────────────────────────────────────────────────────────────────
# MiSTerFin only ever plays a network stream (Jellyfin's HTTP transcode) —
# no physical disc support needed, so this build skips libdvdcss/libdvdread/
# libdvdnav entirely (and their autotools build deps).
echo "=== Building MPlayer $MPLAYER_VER ==="
wget -q https://mplayerhq.hu/MPlayer/releases/MPlayer-$MPLAYER_VER.tar.xz
tar xf MPlayer-$MPLAYER_VER.tar.xz
# Apply vsync patch: wait for blanking interval before each frame write to
# eliminate tearing. The flag file it checks, /tmp/misterdvd_vsync, is
# intentionally left as-is (not ours to rename — src/main.c's own VSYNC_FLAG
# writes/checks that exact path, and this binary is what watches for it).
cp /build/vo_fbdev.c MPlayer-$MPLAYER_VER/libvo/vo_fbdev.c
cd MPlayer-$MPLAYER_VER

./configure \
    --target=arm-linux-gnueabihf \
    --cc=arm-linux-gnueabihf-gcc \
    --as=arm-linux-gnueabihf-gcc \
    --ar=arm-linux-gnueabihf-ar \
    --nm=arm-linux-gnueabihf-nm \
    --ranlib=arm-linux-gnueabihf-ranlib \
    --prefix=$PREFIX \
    --extra-cflags="-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2 -I$PREFIX/include" \
    --extra-ldflags="-L$PREFIX/lib" \
    --disable-mencoder \
    --disable-x11 \
    --disable-xv \
    --disable-xvmc \
    --disable-gl \
    --disable-sdl \
    --disable-gui \
    --disable-png \
    --disable-jpeg \
    --disable-gif \
    --disable-lirc \
    --disable-lircc \
    --disable-joystick \
    --disable-tv \
    --disable-pvr \
    --disable-radio \
    --disable-ossaudio \
    --disable-arts \
    --disable-esd \
    --disable-nas \
    --disable-openal \
    --disable-jack \
    --disable-ladspa \
    --disable-libdv \
    --disable-speex \
    --disable-theora \
    --disable-toolame \
    --disable-twolame \
    --disable-dvdnav \
    --disable-dvdread \
    --enable-fbdev \
    --enable-alsa

make -j$(nproc)

arm-linux-gnueabihf-strip mplayer
cp mplayer /build/mplayer-arm
echo "=== Done: /build/mplayer-arm ==="
