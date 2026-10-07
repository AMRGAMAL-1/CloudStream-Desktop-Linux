# CloudStream Desktop App

This module contains the primary CloudStream Desktop client, built using Compose for Desktop and Kotlin Multiplatform.

## Overview

Unlike the Android application, this module operates in a standard JVM desktop environment. To run plugins designed for Android, the client integrates with `:plugin-runtime` for Dalvik DEX-to-JVM transpilation and `:android-stubs` for Android platform compatibility.

## Architecture Guidelines

- **UI Framework:** All UI is written in Compose Multiplatform following an MVI architecture with reactive StateFlows.
- **Unified Dialog System:** All popups and dialogs MUST use `CloudstreamAlertDialog` or `CloudstreamCustomDialog` from `com.lagradost.cloudstream3.desktop.ui.components.CloudstreamDialogs` to maintain visual consistency and Amoled Pure Black theme support.
- **Thread Safety:** Database writes and file I/O must always run on background dispatchers (`Dispatchers.IO`).
- **Compilation:** Use `launch.bat` (or `launch.bat dev` / `launch.bat build`) in the root directory for development and packaging.

## Linux native player layer

Linux keeps the shared Compose player, original `player.html`/`player.css`/`player.js`,
and MPV flow. The platform bridge uses WebKitGTK 4.1 for the embedded WebView host.
The current AWT Canvas host is X11/XWayland based; native Wayland embedding remains a
separate platform boundary because MPV's current `wid` renderer contract and AWT Canvas
do not expose a native Wayland child-surface API.

Install the system dependencies before building the Linux bridge:

```bash
sudo apt install build-essential cmake pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libmpv-dev
```

`libVLC` is loaded dynamically at runtime, so its development headers are not
required by the current bridge. Install the distribution's VLC runtime when
using the VLC backend; `libvlc-dev` is optional for future native VLC work.

The packaged application does not need the compiler or development headers. It
does need these runtime libraries from the distribution: GTK 3, WebKitGTK 4.1,
libmpv, X11, OpenGL/EGL, and (only when VLC is selected) libVLC. The current
embedded surface is an X11/XWayland child surface; on Wayland, XWayland and a
valid `DISPLAY` are required. A pure native-Wayland session without XWayland is
not supported by this AWT/GTK bridge yet.

Run the repository preflight before distributing a build:

```bash
bash desktop-app/check-linux-dependencies.sh
```

The script checks library sonames rather than Debian/Fedora/Arch package names,
because package names differ between distributions. Suggested package groups:

| Family | Runtime | Build from source |
| --- | --- | --- |
| Debian/Ubuntu/Deepin | `libgtk-3-0 libwebkit2gtk-4.1-0 libmpv2 libvlc5 libx11-6 libgl1 libegl1 xwayland` | `build-essential cmake pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libmpv-dev libvlc-dev` |
| Fedora | `gtk3 webkit2gtk4.1 mpv-libs vlc-libs libX11 mesa-libGL mesa-libEGL xorg-x11-server-Xwayland` | `gcc-c++ cmake pkgconf-pkg-config gtk3-devel webkit2gtk4.1-devel mpv-devel vlc-devel` |
| Arch | `gtk3 webkit2gtk-4.1 mpv vlc libx11 mesa xorg-xwayland` | packages are built from the same package set; install the distro's base-devel toolchain |

Codec availability is supplied by the system GStreamer/VLC/FFmpeg packages;
install the distribution's normal GStreamer codec bundle when WebKit previews
or a particular stream format is unavailable.

## Application update channel

The Windows build continues to use the upstream repository. The Linux build
never checks that repository: it only accepts releases from the Linux fork
configured as `LINUX_UPDATE_REPO` in `AppConfig.kt`, and only tags matching
`linux-v<version>` (for example `linux-v0.1.10`). This separation is strict so
an upstream Windows release cannot appear as a Linux update. Leave the Linux
repository empty until the fork has been published; update checks are then
disabled safely rather than using the wrong channel.

Build the JNI bridge from the repository root:

```bash
cd desktop-app/src/main/cpp
JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")" bash build_jni.sh
```

Then run the desktop application with the normal Gradle task. On Linux the Gradle
`processResources` task also rebuilds the bridge automatically:

```bash
bash ./gradlew :desktop-app:run
```

### Linux diagnostics and packaging

The distributable runtime exposes two read-only support commands:

```bash
./desktop-app/build/compose/binaries/main/app/CloudStream-Desktop/bin/CloudStream-Desktop --version
./desktop-app/build/compose/binaries/main/app/CloudStream-Desktop/bin/CloudStream-Desktop --diagnostics
```

Build the runtime and Linux packages from the repository root:

```bash
./gradlew :desktop-app:createDistributable -PAPP_VERSION=0.1.9 --no-daemon
bash desktop-app/packaging/build-linux-packages.sh --tar
bash desktop-app/packaging/build-linux-packages.sh --deb
bash desktop-app/packaging/build-linux-packages.sh --rpm
```

Use `--all` in CI after installing `appimagetool` to build TAR, DEB, RPM, and
AppImage artifacts. Outputs are written to
`desktop-app/build/outputs/linux/`; the package layout intentionally keeps
GTK/WebKitGTK, libmpv, libVLC, and GPU drivers host-provided.
