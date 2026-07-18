FROM debian:bullseye

# Multiarch for armhf headers
RUN dpkg --add-architecture armhf

RUN apt-get update && apt-get install -y \
    gcc-arm-linux-gnueabihf \
    libc6-dev-armhf-cross \
    binutils-arm-linux-gnueabihf \
    libasound2-dev:armhf \
    wget curl make yasm nasm \
    pkg-config \
    bzip2 xz-utils ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS=arm-linux-gnueabihf
ENV CC=${CROSS}-gcc
ENV CXX=${CROSS}-g++
ENV AR=${CROSS}-ar
ENV RANLIB=${CROSS}-ranlib
ENV STRIP=${CROSS}-strip
ENV PKG_CONFIG=${CROSS}-pkg-config
ENV PREFIX=/build/sysroot

WORKDIR /build

COPY build-mplayer.sh /build/build-mplayer.sh
COPY vo_fbdev.c /build/vo_fbdev.c
RUN chmod +x /build/build-mplayer.sh

CMD ["/build/build-mplayer.sh"]
