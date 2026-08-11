#!/bin/sh
set -eu

case "$*" in
    *libcurl*)
        export PKG_CONFIG_LIBDIR=/work/tools/pkgconfig
        exec /usr/bin/pkg-config "$@"
        ;;
    *)
        export PKG_CONFIG_DIR=
        export PKG_CONFIG_PATH=
        export PKG_CONFIG_SYSROOT_DIR=
        export PKG_CONFIG_LIBDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig
        if [ "${1:-}" = "--version" ]; then
            exec /usr/bin/pkg-config --version
        fi
        exec /usr/bin/pkg-config --static "$@"
        ;;
esac
