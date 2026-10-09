#!/usr/bin/env bash
# A shell script to compile the JNI native bridge for CloudStream Desktop on Linux.
# Requirements: a C++17 compiler, a JDK, pkg-config, GTK 3, WebKitGTK 4.1,
# and libmpv development headers. libmpv itself is loaded by the JVM/native
# bridge at runtime and is deliberately not linked into DT_NEEDED here.

set -e

SO_DIR="../../../appResources/linux/jni"
mkdir -p "$SO_DIR"
SO_OUTPUT="$SO_DIR/libplayer_bridge.so"

if [ -z "${JAVA_HOME:-}" ] && command -v javac >/dev/null 2>&1; then
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
    export JAVA_HOME
fi

if [ -z "${JAVA_HOME:-}" ] || [ ! -f "$JAVA_HOME/include/jni.h" ]; then
    echo "ERROR: JAVA_HOME must point to a JDK containing include/jni.h." >&2
    exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "ERROR: pkg-config is required to locate GTK/WebKitGTK/MPV." >&2
    exit 1
fi

if ! pkg-config --exists gtk+-3.0 webkit2gtk-4.1 mpv; then
    echo "ERROR: Missing build metadata. Required pkg-config modules: gtk+-3.0 webkit2gtk-4.1 mpv." >&2
    echo "       Install the distribution's GTK3, WebKitGTK 4.1, and libmpv development packages." >&2
    exit 1
fi

JAVA_INCLUDE="$JAVA_HOME/include"
JAVA_INCLUDE_LINUX="$JAVA_HOME/include/linux"
COMMON_INCLUDE="include"
GTK_FLAGS="$(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1)"
MPV_FLAGS="$(pkg-config --cflags mpv)"
# libswscale converts VLC's planar I420 frames to displayable BGRA in our own
# code, bypassing VLC's chroma-converter chain (observed crash site).
SWSCALE_FLAGS="$(pkg-config --cflags --libs libswscale)"
# XComposite for the offscreen controls-window redirect (composite-redirected
# overlay). Required at link time now that -z defs rejects dangling symbols.
XCOMPOSITE_FLAGS="$(pkg-config --cflags --libs xcomposite)"
CXX="${CXX:-g++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "ERROR: C++ compiler not found: $CXX" >&2
    exit 1
fi

echo "Compiling libplayer_bridge.so for Linux..."

"$CXX" -shared -fPIC -std=c++17 -O2 \
    -Wl,-z,relro,-z,now,-z,defs \
    -o "$SO_OUTPUT" \
    common/mpv_core.cpp \
    linux/surface_linux.cpp \
    -I"$COMMON_INCLUDE" \
    -I"$JAVA_INCLUDE" \
    -I"$JAVA_INCLUDE_LINUX" \
    $MPV_FLAGS \
    $GTK_FLAGS \
    $SWSCALE_FLAGS \
    $XCOMPOSITE_FLAGS \
    -lpthread -ldl -lX11 -lGL

echo "Compilation successful! SO output to: $SO_OUTPUT"
