# CloudStream Desktop (Unofficial Client)

Desktop-native streaming client built with **Compose Multiplatform** for 64-bit Windows and Linux. Runs Android CloudStream extensions natively on a desktop JVM without requiring emulators or compatibility layers.

> [!CAUTION]
> **Active Developer & Experimental Pre-Alpha State**
> * **Developer-Only Environment:** This repository is an active, fast-moving development and experimentation codebase intended strictly for developers and technical testers. It is **not** a stable release and is **not** intended for general or regular end-user consumption.
> * **AI-Assisted Codebase & Instability:** This codebase is actively researched, developed, and refactored with AI assistance. It may contain highly experimental implementations, non-standard patterns, and volatile code.
> * **Zero Feature Stability Guarantees:** Features, internal APIs, storage schemas, and platform behavior undergo rapid iteration and may break or change at any time. Exercise caution when interacting with project files, databases, or local configs.

> [!IMPORTANT]
> **Project Scope & Architecture Directives**
> * **Desktop-Exclusive Hard Fork:** This repository is built for 64-bit desktop platforms. It is an independent hard fork and does not merge upstream into Android CloudStream.
> * **Zero Affiliation:** This project is independent and unaffiliated with the original Android CloudStream application or its development team. Please do not contact upstream developers regarding this client.
> * **Ad-Free Policy:** Strict ad-free project. Derivative builds and forks must remain clean, free, and open.


## Architectural Overview

The application is structured into modular subprojects separating platform abstraction, runtime transcompilation, and UI presentation:

| Module | Responsibility |
| :--- | :--- |
| **`:desktop-app`** | Compose Multiplatform presentation layer, Amoled dark theme, local stream proxy, and native desktop window controls. |
| **`:plugin-runtime`** | Transcompilation engine. Converts Dalvik DEX bytecode into JVM bytecode via Dex2jar, applies ASM bytecode instrumentation, and enforces sandbox security policies. |
| **`:player-abstraction`** | JNA bindings to the native `libmpv` C-core for hardware-accelerated video decoding. |
| **`:android-stubs`** | Stubs for Android platform APIs (`Context`, `SharedPreferences`, `Build`, `DisplayMetrics`) allowing Android extension bytecode to run on the JVM. |
| **`:common`** | SQLite persistence layer powered by **SQLDelight** for local history, preferences, and state management. |
| **`:library`** | Base CloudStream contracts and core provider interfaces. |

---

## Developer Setup & Quick Start

### Prerequisites
* **Operating System:** Windows 10 / 11 or Linux (64-bit)
* **Web Runtime:** Windows uses **Microsoft Edge WebView2 Runtime**; Linux uses system **WebKitGTK 4.1** for the native player host.
* **Java Development Kit:** **JDK 21** or higher (e.g. [Eclipse Adoptium Temurin 21](https://adoptium.net/temurin/releases/?version=21))
* **Git:** Installed and available in PATH

#### Linux runtime support matrix

The Linux player is portable across 64-bit distributions that provide GTK 3,
WebKitGTK 4.1, libmpv, X11/OpenGL/EGL, and JDK 21. Wayland sessions currently
need XWayland because the embedded AWT/GTK surface is an X11 child surface;
native Wayland without XWayland is not supported yet. VLC is an optional native
backend and additionally needs the system libVLC runtime.

For an exact machine check, run:

```bash
bash desktop-app/check-linux-dependencies.sh
```

#### Realistic compatibility matrix

| Distribution family | Package path | Display path | Current evidence | Known limitation |
| --- | --- | --- | --- | --- |
| Debian 12+ / Ubuntu 22.04+ / Deepin 23 / Mint 21+ | DEB, portable TAR | X11; Wayland via XWayland | Runtime verified on Deepin; DEB/TAR built locally; Ubuntu 22.04 CI workflow | Host WebKitGTK 4.1/libmpv sonames must be available |
| Fedora 36+ / RHEL-family / openSUSE Tumbleweed | RPM, portable TAR | X11; Wayland via XWayland | RPM spec and host-provided dependency model prepared | Package names and WebKitGTK availability vary by release; runtime not tested here |
| Arch / Manjaro / EndeavourOS / Pop!_OS / KDE neon / elementary OS 7 | Portable TAR or native repackaging | X11; Wayland via XWayland | Soname-based preflight avoids distro package-name assumptions | Runtime validation on each distribution is still required |
| Debian 11 / Ubuntu 20.04 / openSUSE Leap 15.x | — (build from source only) | X11; Wayland via XWayland | Below the glibc 2.35 baseline; a source build on that system may work | No prebuilt package is provided for this class |
| x86_64 | All local package targets | Supported target | Native bridge and packages validated on x86_64 | Keep the CI-built baseline for distribution |
| aarch64 / ARM64 | DEB, TAR (experimental CI job) | Depends on host X11/XWayland | Packaging selects arm64; optional arm64 CI job builds DEB/TAR | No media-playback validation on ARM GPUs yet; treat as experimental |

The current locally built bridge reports GLIBC_2.34 and GLIBCXX_3.4.30 as its
minimum observed symbol versions; the Deepin-bundled JDK also raises the
bundled JRE requirement to glibc 2.38. This is evidence from the current
developer build, not the release baseline.

**Official release baseline (produced by CI on Ubuntu 22.04):** glibc >= 2.35,
libstdc++ / GLIBCXX >= 3.4.29 (GCC 11), x86_64. This covers Debian 12+,
Ubuntu 22.04+, Linux Mint 21+, Pop!_OS 22.04+, elementary OS 7+, KDE neon,
Deepin 23, Fedora 36+, Arch/Manjaro/EndeavourOS and openSUSE Tumbleweed.
Debian 11 / Ubuntu 20.04 / openSUSE Leap 15.x (glibc 2.31) are below the
baseline and are **not** supported by prebuilt packages. Always prefer the
CI-built packages for distribution; a package built on a newer toolchain or
JDK silently inherits that host's higher glibc requirement.

The check is based on runtime library sonames, not package names, so it works
across Debian/Ubuntu/Deepin, Fedora, and Arch-style systems. Package names and
installation commands for the main families are documented in
`desktop-app/README.md`.

#### Production Linux release checklist

```bash
# 1. Build the native bridge, run all JVM tests, and produce the runtime image
./gradlew :plugin-runtime:test :player-abstraction:test :desktop-app:test \
    :desktop-app:linuxNativeBridge :desktop-app:createDistributable \
    --no-daemon -PAPP_VERSION="$APP_VERSION"

# 2. Preflight the host dependencies (soname-based)
bash desktop-app/check-linux-dependencies.sh

# 3. Build TAR + DEB + RPM (AppImage needs appimagetool and is built in CI)
SKIP_BUILD=1 bash desktop-app/packaging/build-linux-packages.sh --all

# 4. Validate the runtime image (ELF arch, ldd, cfg paths, desktop entry, --version, --diagnostics)
SKIP_BUILD=1 bash desktop-app/packaging/build-linux-packages.sh --validate
desktop-file-validate desktop-app/packaging/linux/com.cloudstream.CloudStreamDesktop.desktop

# 5. Install/uninstall smoke test (Debian-family, requires sudo)
sudo dpkg -i desktop-app/build/outputs/linux/cloudstream-desktop_*.deb
cloudstream --version && cloudstream --diagnostics
sudo dpkg -r cloudstream-desktop

# 6. Tag a release; CI then builds, tests, packages and uploads all formats
git tag linux-v0.1.9 && git push origin linux-v0.1.9
```

The packaged launcher resolves the native bridge from
`lib/app/resources/jni/libplayer_bridge.so` inside the runtime image. The
`cloudstream` launcher (DEB/RPM) and `AppRun` (AppImage/tar.gz) export
`VLC_PLUGIN_PATH` when a bundled plugin directory exists; otherwise libVLC
discovers the system plugins through `libvlccore`.

### Linux update channel

Linux builds use the Linux fork's GitHub Releases only and never read the
upstream Windows release channel. The channel is configured to
`AMRGAMAL-1/CloudStream-Desktop-Linux` (see `LINUX_UPDATE_REPO` in
`desktop-app/src/main/kotlin/com/lagradost/cloudstream3/desktop/AppConfig.kt`;
dev builds can override it with `-Dcloudstream.linux.update.repo=owner/repo`).
Publish Linux releases with tags such as `linux-v0.1.10`; the Linux client
accepts only `linux-v<version>` tags, so Windows releases can never be
offered to Linux users. Until the first tagged Linux release exists, update
checks simply find nothing and stay quiet by design.

---

### Step 1: Clone With Submodules
This repository relies on internal submodules. You **must** clone recursively:

```bash
git clone --recursive https://github.com/errorcode26/CS3-desktop-client-unofficial.git
cd CS3-desktop-client-unofficial
```

*(If you already cloned without `--recursive`, run `git submodule update --init --recursive` inside the repository).*

---

### Step 2: Native Binaries Setup (MPV)
On Linux, install the native dependencies instead of downloading Windows DLLs:

```bash
sudo apt install libmpv-dev libgtk-3-dev libwebkit2gtk-4.1-dev pkg-config
```

The Linux JNI bridge is built automatically by Gradle on Linux, or manually with:

```bash
cd desktop-app/src/main/cpp
JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")" bash build_jni.sh
```

The current embedded AWT/MPV surface is validated through X11/XWayland. Native
Wayland child-surface embedding remains a separate platform boundary because the
existing MPV `wid` contract is X11-based.

On Windows, the video player requires the 64-bit native `libmpv-2.dll` placed in `desktop-app/appResources/windows/mpv/`.

Because `libmpv-2.dll` (~112 MB) exceeds GitHub's 100 MB single-file repository limit, it is not bundled directly in git.

| Binary | Distribution | Tracked in Git? | Purpose |
| :--- | :--- | :---: | :--- |
| **`libmpv-2.dll`** (~112 MB) | Downloaded manually | No | Native MPV media playback core |
| **`player_bridge.dll`** (~180 KB) | Pre-bundled | Yes | Win32 Airspace HWND compositor & low-latency fast-path |
| **`WebView2Loader.dll`** (~160 KB) | Pre-bundled | Yes | Microsoft WebView2 runtime dynamic loader |

**How to get `libmpv-2.dll`:**
1. Download the `mpv-dev-x86_64-*.7z` development package from [shinchiro/mpv-winbuild-cmake releases](https://github.com/shinchiro/mpv-winbuild-cmake/releases) (such as pinned build [20260610](https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20260610)).
2. Extract `libmpv-2.dll` from the downloaded archive.
3. Place `libmpv-2.dll` into:

```text
desktop-app/
└── appResources/
    └── windows/
        ├── mpv/
        │   ├── libmpv-2.dll          <-- Place extracted DLL here
        │   └── portable_config/
        │       └── mpv.conf
        └── jni/
            ├── player_bridge.dll     <-- Pre-bundled in repository
            └── WebView2Loader.dll    <-- Pre-bundled in repository
```

---

### Step 3: Run & Build

Use the interactive launcher script:

```bat
.\launch.bat
```

The launcher provides quick shortcuts:
* `.\launch.bat dev` — Start the client with Live LogCat (F12) enabled.
* `.\launch.bat release` — Launch the compiled standalone executable.
* `.\launch.bat build` — Compile the standalone distribution EXE via Gradle.
* `.\launch.bat test` — Run all module test suites and compile checks.

Alternatively, execute tasks directly via Gradle:
```bat
# Run in dev mode
.\gradlew.bat :desktop-app:run --args="--dev"

# Compile standalone distributable
.\gradlew.bat :desktop-app:createDistributable
```

---

## Native Bridge & Player Architecture

### The Win32 Airspace & Fast-Path Architecture
Directly overlaying Java Swing / Compose Multiplatform components onto a native video window handle (`HWND`) causes severe visual occlusion and flicker (the Win32 "Airspace problem"). Furthermore, routing high-frequency seekbar scrubs through JVM garbage collection and JNI layers introduces 10–50ms latency delays that cause scrubber rubber-banding.

CloudStream Desktop resolves this through a hybrid native architecture in `desktop-app/src/main/cpp/player_bridge.cpp`:
1. **HWND Composition:** A unified native Win32 container (`g_containerHwnd`) hosts MPV's render context as the base layer, with a transparent Microsoft WebView2 Chromium instance layered directly on top.
2. **C++ Native Fast-Path:** Timeline scrubbing, volume changes, and state events are intercepted and processed directly inside C++ in sub-millisecond time (`g_mpv_command_string`), completely bypassing JVM overhead.
3. **High-Frequency Polling:** A native timer polls MPV playback positions (`time-pos`, `bufferPos`) and pushes updates via `PostWebMessageAsJson` without allocating JVM objects.
4. **Cloudflare CDP Resolution:** WebView2 is also leveraged headlessly by `CloudflareKiller.kt` via Chrome DevTools Protocol to solve Cloudflare Turnstile / anti-bot challenges that Android extensions expect an Android OS WebView to handle.

### Optional: Recompiling the Native C++ Bridge
Developers only need a C++ compiler if modifying `desktop-app/src/main/cpp/player_bridge.cpp`. The pre-compiled DLLs are already tracked in git.

To recompile:
* **MinGW-w64 (GCC):** Ensure `g++` and `JAVA_HOME` are set, then execute:
  ```powershell
  cd desktop-app\src\main\cpp
  .\build_jni.ps1
  ```
  *(Statically links `libstdc++` and `libwinpthread` to eliminate external runtime dependencies).*
* **CMake (MSVC / CLion):** A standard `CMakeLists.txt` is provided in `desktop-app/src/main/cpp/` linking the self-contained WebView2 headers and libraries in `webview2/`.

---

## Disclaimer

This software is an empty media player shell and runtime harness. It does not host, distribute, or bundle any media content, streams, or scrapers. Users are solely responsible for extensions they choose to install.
