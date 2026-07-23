#!/bin/bash
# MiSTerFin launcher

APP=/media/fat/misterfin/misterfin-arm

if [ -f /tmp/misterfin_apply_update.sh ]; then
    bash /tmp/misterfin_apply_update.sh
    rm -f /tmp/misterfin_apply_update.sh
fi

cd /media/fat/misterfin
exec "$APP"
