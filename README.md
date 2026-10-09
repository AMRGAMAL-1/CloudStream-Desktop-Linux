# CloudStream Desktop for Linux

Unofficial native CloudStream desktop client for Linux.

Built with **Compose Multiplatform** for 64-bit Linux. Runs Android CloudStream extensions natively on a desktop JVM — no emulators, no compatibility layers.

> [!NOTE]
> **Beta / Early Release.** This project is under active development and some features may change between releases. It is usable today, but expect rough edges and report issues with the output of `cloudstream --diagnostics`.

> [!IMPORTANT]
> **Independent, ad-free hard fork.** This repository is a desktop-exclusive hard fork and does not merge upstream into Android CloudStream. It is unaffiliated with the original Android CloudStream app and its team — please do not contact upstream developers about this client. Derivative builds and forks must remain clean, free, and open.

---

## Features

- Native desktop client (Compose Multiplatform, Amoled dark theme, desktop window controls)
- Runs Android CloudStream extensions on the JVM (DEX-to-JVM transcompilation + sandbox)
- Hardware-accelerated playback via system `libmpv`, with optional system `libVLC` backend
- Embedded WebKitGTK player surface reusing the shared `player.html` / `player.css` / `player.js`
- Local SQLite persistence (history, preferences, state) via SQLDelight
- `cloudstream --version` / `cloudstream --diagnostics` support commands for bug reports
- Strict Linux-only update channel: only `linux-v<version>` tags from this repository are ever offered as updates

## Requirements

- 64-bit Linux (x86_64; arm64 packages are experimental, see below)
- JDK 21 or higher (e.g. [Eclipse Adoptium Temurin 21](https://adoptium.net/temurin/releases/?version=21))
- Git (only for building from source)
- Host runtime libraries (see table below)

| Library | Purpose |
| --- | --- |
| GTK 3 | Native window / embedded player surface |
| WebKitGTK 4.1 | Embedded WebView player host |
| libmpv (`.so.2` or `.so.1`) | Hardware-accelerated video decoding |
| X11 / OpenGL / EGL | Rendering (`libX11`, `libGL`, `libEGL`) |
| XWayland + `DISPLAY` | Required on Wayland sessions (see below) |
| libVLC (optional) | Only needed when the VLC backend is selected |

For an exact machine check, run:

```bash
bash desktop-app/check-linux-dependencies.sh
```

The check is soname-based (not package-name-based), so it works across Debian/Ubuntu/Deepin, Fedora, and Arch-style systems. Suggested package groups per family are documented in [`desktop-app/README.md`](desktop-app/README.md).

## Supported distributions & environments

**Official release baseline (CI-built on Ubuntu 22.04):** glibc >= 2.35, libstdc++ / GLIBCXX >= 3.4.29 (GCC 11), x86_64. Always prefer the CI-built packages for distribution — a package built on a newer toolchain or JDK silently inherits that host's higher glibc requirement.

| Distribution family | Package path | Display path | Notes |
| --- | --- | --- | --- |
| Debian 12+ / Ubuntu 22.04+ / Deepin 23 / Mint 21+ | DEB, portable TAR | X11; Wayland via XWayland | Primary path; Ubuntu 22.04 CI workflow |
| Fedora 36+ / RHEL-family / openSUSE Tumbleweed | RPM, portable TAR | X11; Wayland via XWayland | Host-provided dependency model; package names vary by release |
| Arch / Manjaro / EndeavourOS / Pop!_OS / KDE neon / elementary OS 7 | Portable TAR or native repackaging | X11; Wayland via XWayland | Soname preflight avoids distro package-name assumptions |
| Debian 11 / Ubuntu 20.04 / openSUSE Leap 15.x | Build from source only | X11; Wayland via XWayland | Below the glibc 2.35 baseline; no prebuilt package is provided |
| x86_64 | All package formats | Supported target | Validated target |
| aarch64 / ARM64 | DEB, TAR (experimental) | Depends on host X11/XWayland | Packaging selects arm64; no media-playback validation on ARM GPUs yet |

**Wayland:** the embedded AWT/GTK surface is an X11 child surface, so Wayland sessions currently need XWayland. A pure native-Wayland session without XWayland is not supported yet.

## Installation

Download the latest release from [GitHub Releases](https://github.com/AMRGAMAL-1/CloudStream-Desktop-Linux/releases) (titled `CloudStream <version>`, e.g. **CloudStream 0.1.9**).

```bash
# Debian / Ubuntu / Deepin / Mint
sudo dpkg -i cloudstream-desktop_0.1.9_amd64.deb
cloudstream --version

# Fedora / RHEL-family / openSUSE
sudo rpm -i cloudstream-desktop-0.1.9-1.x86_64.rpm
cloudstream --version

# Portable TAR (any supported distro)
tar -xzf CloudStream-Desktop-0.1.9-linux-x86_64.tar.gz
./CloudStream-Desktop/bin/CloudStream-Desktop --version
# or use the bundled launcher: ./cloudstream --version

# AppImage (any supported distro)
chmod +x CloudStream-Desktop-0.1.9-linux-x86_64.AppImage
./CloudStream-Desktop-0.1.9-linux-x86_64.AppImage --version
```

Uninstall:

```bash
# DEB
sudo dpkg -r cloudstream-desktop

# RPM
sudo rpm -e cloudstream-desktop
```

## Available packages

Every release publishes four artifacts for x86_64 (filenames use the numeric version, e.g. `0.1.9`):

| Format | Filename pattern | Notes |
| --- | --- | --- |
| DEB | `cloudstream-desktop_<version>_amd64.deb` | Technical package ID stays lowercase (`cloudstream-desktop`) so upgrades keep working; installs to `/opt/cloudstream/CloudStream-Desktop`, launcher is `cloudstream` |
| RPM | `cloudstream-desktop-<version>-1.x86_64.rpm` | Same layout and launcher as the DEB |
| TAR.GZ | `CloudStream-Desktop-<version>-linux-x86_64.tar.gz` | Portable; run `CloudStream-Desktop/bin/CloudStream-Desktop` or the bundled `cloudstream` launcher |
| AppImage | `CloudStream-Desktop-<version>-linux-x86_64.AppImage` | Portable; leaves GTK/WebKitGTK, libmpv, libVLC and GPU drivers host-provided |

The menu entry is **CloudStream Desktop** (`com.cloudstream.CloudStreamDesktop.desktop`). The display name uses the `CloudStream` capitalization; the lowercase `cloudstream-desktop` package ID and `/opt/cloudstream` paths are intentionally kept stable for existing installations.

## Build from source

```bash
# 1. Clone (this repository relies on submodules — clone recursively)
git clone --recursive https://github.com/AMRGAMAL-1/CloudStream-Desktop-Linux.git
cd CloudStream-Desktop-Linux
# (If you already cloned without --recursive: git submodule update --init --recursive)

# 2. Install build dependencies (Debian/Ubuntu example)
sudo apt install build-essential cmake pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libmpv-dev

# 3. Build the native bridge, run all JVM tests, and produce the runtime image
./gradlew :plugin-runtime:test :player-abstraction:test :desktop-app:test \
    :desktop-app:linuxNativeBridge :desktop-app:createDistributable \
    --no-daemon -PAPP_VERSION="0.1.9"

# 4. Preflight the host dependencies
bash desktop-app/check-linux-dependencies.sh

# 5. Build TAR + DEB + RPM (AppImage needs appimagetool and is built in CI)
SKIP_BUILD=1 bash desktop-app/packaging/build-linux-packages.sh --all

# 6. Validate the runtime image
SKIP_BUILD=1 bash desktop-app/packaging/build-linux-packages.sh --validate
desktop-file-validate desktop-app/packaging/linux/com.cloudstream.CloudStreamDesktop.desktop
```

To build the JNI bridge manually:

```bash
cd desktop-app/src/main/cpp
JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")" bash build_jni.sh
```

Run in development mode:

```bash
./gradlew :desktop-app:run
```

The packaged launcher resolves the native bridge from `lib/app/resources/jni/libplayer_bridge.so` inside the runtime image. The `cloudstream` launcher (DEB/RPM) and `AppRun` (AppImage/TAR) export `VLC_PLUGIN_PATH` when a bundled plugin directory exists; otherwise libVLC discovers the system plugins through `libvlccore`.

## Known issues / limitations

- **Wayland without XWayland is not supported.** On Wayland sessions, XWayland and a valid `DISPLAY` are required because the embedded surface is X11-based.
- **Host libraries are not bundled.** GTK 3, WebKitGTK 4.1, libmpv, X11/OpenGL/EGL (and libVLC when selected) must come from the distribution; missing ones are reported by `check-linux-dependencies.sh` and `cloudstream --diagnostics`.
- **Below-baseline distros** (Debian 11 / Ubuntu 20.04 / openSUSE Leap 15.x, glibc 2.31) cannot use the prebuilt packages — build from source on that system instead.
- **ARM64 is experimental.** DEB/TAR packaging selects arm64, but media playback on ARM GPUs is not validated yet.
- **Codec gaps** come from the system GStreamer/VLC/FFmpeg packages — install the distribution's normal codec bundle when a preview or stream format is unavailable.
- Storage schemas and internal APIs may still change between early releases.

## Updating

Linux builds check **only** this repository's releases (`AMRGAMAL-1/CloudStream-Desktop-Linux`) and accept **only** tags of the form `linux-v<version>` (e.g. `linux-v0.1.9`). The `linux-` prefix is the internal Git tag format the updater depends on; the version shown to users is always the plain numeric version (`0.1.9`), and release titles read **CloudStream 0.1.9**. Until a tagged Linux release exists, update checks simply find nothing and stay quiet by design. Dev builds can override the channel with `-Dcloudstream.linux.update.repo=owner/repo`.

To publish a release, maintainers push a tag; CI then builds, tests, packages and uploads all formats:

```bash
git tag linux-v0.1.9 && git push origin linux-v0.1.9
```

## Versioning

- **User-facing version:** numeric, e.g. `0.1.9` (single source of truth: `APP_VERSION` in `gradle.properties`).
- **Git tag:** `linux-v0.1.9` (required by the updater's `linux-v` prefix check — do not change one without the other).
- **GitHub Release title:** `CloudStream 0.1.9`.
- **Artifacts:** `CloudStream-Desktop-0.1.9-linux-x86_64.tar.gz` / `.AppImage`, `cloudstream-desktop_0.1.9_amd64.deb`, `cloudstream-desktop-0.1.9-1.x86_64.rpm`.

## Disclaimer

This software is an empty media player shell and runtime harness. It does not host, distribute, or bundle any media content, streams, or scrapers. Users are solely responsible for extensions they choose to install.

## Credits / Acknowledgements

- [CloudStream (Android)](https://github.com/recloudstream/cloudstream) — the upstream Android app whose core library powers this client (unaffiliated; do not contact upstream about this fork).
- [Extension documentation](https://recloudstream.github.io/csdocs/) — plugin development guides and extension APIs.
- Compose Multiplatform, SQLDelight, libmpv, WebKitGTK and the Linux distribution maintainers whose runtimes this client builds on.
- Linux edition maintainer: AMR GAMAL.
