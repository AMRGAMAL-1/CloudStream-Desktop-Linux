#!/usr/bin/env bash
set -euo pipefail

bundle="${1:-}"
if [[ -z "$bundle" ]]; then
    echo "Usage: $0 <CloudStream-Desktop runtime image>" >&2
    exit 2
fi

launcher="$bundle/bin/CloudStream-Desktop"
bridge="$bundle/lib/app/resources/jni/libplayer_bridge.so"
cfg="$bundle/lib/app/CloudStream-Desktop.cfg"
desktop_file="$(dirname "${BASH_SOURCE[0]}")/com.cloudstream.CloudStreamDesktop.desktop"

[[ -x "$launcher" ]] || { echo "[FAIL] missing launcher: $launcher"; exit 1; }
[[ -f "$bridge" ]] || { echo "[FAIL] missing native bridge: $bridge"; exit 1; }
[[ -f "$cfg" ]] || { echo "[FAIL] missing jpackage config: $cfg"; exit 1; }

# The bridge and the app resources must be reachable from the packaged
# launcher. In this jpackage layout $APPDIR resolves to lib/app, so the
# resources dir is $APPDIR/resources and the CDS archive is $APPDIR/app.jsa.
if ! grep -q 'java.library.path=\$APPDIR/resources/jni' "$cfg"; then
    echo "[FAIL] packaged java.library.path does not point at resources/jni"
    grep 'java.library.path' "$cfg" || true
    exit 1
fi
if ! grep -q 'compose.application.resources.dir=\$APPDIR/resources' "$cfg"; then
    echo "[FAIL] compose.application.resources.dir is not $APPDIR/resources"
    grep 'compose.application.resources.dir' "$cfg" || true
    exit 1
fi
if ! grep -q 'SharedArchiveFile=\$APPDIR/app.jsa' "$cfg"; then
    echo "[FAIL] packaged AppCDS archive flag is missing"
    grep 'SharedArchiveFile' "$cfg" || true
    exit 1
fi
if ! grep -q 'app.classpath=\$APPDIR/' "$cfg"; then
    echo "[FAIL] packaged classpath does not use \$APPDIR/"
    grep 'app.classpath' "$cfg" | head -1 || true
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64) expected_machine="x86-64" ;;
    aarch64|arm64) expected_machine="ARM aarch64" ;;
    *) echo "[FAIL] unsupported validation architecture: $(uname -m)"; exit 1 ;;
esac

launcher_file="$(file -b "$launcher")"
bridge_file="$(file -b "$bridge")"
printf 'Launcher: %s\nBridge: %s\n' "$launcher_file" "$bridge_file"
[[ "$launcher_file" == *"$expected_machine"* ]] || {
    echo "[FAIL] launcher architecture does not match the host ($(uname -m))"
    exit 1
}
[[ "$bridge_file" == *"$expected_machine"* ]] || {
    echo "[FAIL] native bridge architecture does not match the host ($(uname -m))"
    exit 1
}

if ldd "$bridge" | grep -q 'not found'; then
    echo "[FAIL] unresolved native bridge dependency"
    ldd "$bridge"
    exit 1
fi

if ! ldd -r "$bridge" >/dev/null 2>&1; then
    echo "[FAIL] native bridge has unresolved runtime symbols"
    ldd -r "$bridge" || true
    exit 1
fi

# Report the ABI baseline so a package is not silently shipped with a newer
# glibc/libstdc++ requirement than the declared minimum (2.35 / GLIBCXX 3.4.29).
bridge_glibc="$(readelf -V "$bridge" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -n 1)"
bridge_glibcxx="$(readelf -V "$bridge" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | tail -n 1)"
launcher_glibc="$(readelf -V "$launcher" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -n 1)"
printf 'ABI baseline: bridge %s / %s, launcher %s\n' "${bridge_glibc:-unknown}" "${bridge_glibcxx:-unknown}" "${launcher_glibc:-unknown}"
if [[ -n "$bridge_glibc" && "$bridge_glibc" > "GLIBC_2.35" ]]; then
    echo "[WARN] bridge requires $bridge_glibc, above the declared 2.35 baseline"
fi
if [[ -n "$bridge_glibcxx" && "$bridge_glibcxx" > "GLIBCXX_3.4.29" ]]; then
    echo "[WARN] bridge requires $bridge_glibcxx, above the declared GCC 11 baseline"
fi

if readelf -d "$bridge" | grep -qE 'RPATH|RUNPATH'; then
    echo "[WARN] bridge contains RPATH/RUNPATH; review portability before release"
fi

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$desktop_file"
else
    echo "[WARN] desktop-file-validate is not installed; skipped desktop entry validation"
fi

tmp_home="$(mktemp -d)"
trap 'rm -rf "$tmp_home"' EXIT
HOME="$tmp_home" XDG_CONFIG_HOME="$tmp_home/config" XDG_DATA_HOME="$tmp_home/data" \
    XDG_CACHE_HOME="$tmp_home/cache" XDG_STATE_HOME="$tmp_home/state" \
    "$launcher" --version
HOME="$tmp_home" XDG_CONFIG_HOME="$tmp_home/config" XDG_DATA_HOME="$tmp_home/data" \
    XDG_CACHE_HOME="$tmp_home/cache" XDG_STATE_HOME="$tmp_home/state" \
    "$launcher" --diagnostics

echo "[ OK ] Linux runtime image validation passed"
