#!/usr/bin/env bash
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bridge_path="${CLOUDSTREAM_BRIDGE:-$script_dir/appResources/linux/jni/libplayer_bridge.so}"

# Runtime/build preflight for CloudStream Desktop Linux.
# This intentionally checks sonames rather than distro package names because
# Debian, Fedora, Arch, and Deepin name the same runtime packages differently.

required_runtime=(
  "libgtk-3.so.0|GTK 3"
  "libwebkit2gtk-4.1.so.0|WebKitGTK 4.1"
  "libX11.so.6|X11"
  "libGL.so.1|OpenGL"
  "libEGL.so.1|EGL"
  "libstdc++.so.6|libstdc++ (GCC 11 or newer)"
)
mpv_runtime=("libmpv.so.2" "libmpv.so.1")
optional_runtime=(
  "libvlc.so.5|libVLC (required only when VLC is selected)"
  "libwayland-client.so.0|Wayland client (only for a Wayland session)"
)
build_tools=(
  "pkg-config|pkg-config"
  "cmake|CMake"
  "g++|GNU C++ compiler"
)
build_modules=(
  "gtk+-3.0|GTK 3 development metadata"
  "webkit2gtk-4.1|WebKitGTK 4.1 development metadata"
  "mpv|libmpv development metadata"
)

ok=0
warn=0
fail=0

has_library() {
  local soname="$1"
  if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -Fq "${soname} ("; then
    return 0
  fi
  find /lib /usr/lib /lib64 /usr/lib64 -type f -name "${soname}" -print -quit 2>/dev/null | grep -q .
}

check_command() {
  local command_name="$1"
  command -v "${command_name}" >/dev/null 2>&1
}

check_runtime_list() {
  local severity="$1"
  shift
  local item soname label
  for item in "$@"; do
    soname="${item%%|*}"
    label="${item#*|}"
    if has_library "${soname}"; then
      printf '[ OK ] %s (%s)\n' "${label}" "${soname}"
      ok=$((ok + 1))
    elif [[ "${severity}" == required ]]; then
      printf '[FAIL] %s is missing (%s)\n' "${label}" "${soname}"
      fail=$((fail + 1))
    else
      printf '[WARN] %s is missing (%s)\n' "${label}" "${soname}"
      warn=$((warn + 1))
    fi
  done
}

check_runtime_alternatives() {
  local label="$1"
  shift
  local soname
  local alternatives=""
  for soname in "$@"; do
    if [[ -n "${alternatives}" ]]; then
      alternatives+=" or "
    fi
    alternatives+="${soname}"
    if has_library "${soname}"; then
      printf '[ OK ] %s (%s)\n' "${label}" "${soname}"
      ok=$((ok + 1))
      return 0
    fi
  done
  printf '[FAIL] %s is missing (%s)\n' "${label}" "${alternatives}"
  fail=$((fail + 1))
  return 1
}

printf 'CloudStream Desktop Linux dependency check\n'
printf '===========================================\n'
printf 'Architecture: '; uname -m
printf 'Session: XDG_SESSION_TYPE=%s DISPLAY=%s WAYLAND_DISPLAY=%s\n\n' \
  "${XDG_SESSION_TYPE:-unknown}" "${DISPLAY:-unset}" "${WAYLAND_DISPLAY:-unset}"

printf 'Runtime required by the embedded MPV/WebKit player:\n'
check_runtime_list required "${required_runtime[@]}"
check_runtime_alternatives "libmpv" "${mpv_runtime[@]}"
printf '\nRuntime optional/recommended:\n'
check_runtime_list optional "${optional_runtime[@]}"

if [[ "${XDG_SESSION_TYPE:-}" == wayland && -z "${DISPLAY:-}" ]]; then
  printf '[FAIL] Wayland session has no X11 DISPLAY; the current embedded player needs XWayland.\n'
  fail=$((fail + 1))
elif [[ "${XDG_SESSION_TYPE:-}" == wayland ]]; then
  printf '[ OK ] Wayland session exposes DISPLAY; XWayland embedding is available.\n'
  ok=$((ok + 1))
else
  printf '[ OK ] X11-compatible session detected.\n'
  ok=$((ok + 1))
fi

printf '\nBuild tools (needed only when building the JNI bridge from source):\n'
for item in "${build_tools[@]}"; do
  command_name="${item%%|*}"
  label="${item#*|}"
  if check_command "${command_name}"; then
    printf '[ OK ] %s\n' "${label}"
    ok=$((ok + 1))
  else
    printf '[WARN] %s is not installed\n' "${label}"
    warn=$((warn + 1))
  fi
done

if check_command pkg-config; then
printf '\nBuild metadata:\n'
  for item in "${build_modules[@]}"; do
    module="${item%%|*}"
    label="${item#*|}"
    if pkg-config --exists "${module}"; then
      printf '[ OK ] %s (%s)\n' "${label}" "$(pkg-config --modversion "${module}")"
      ok=$((ok + 1))
    else
      printf '[WARN] %s is missing (%s)\n' "${label}" "${module}"
      warn=$((warn + 1))
    fi
  done
fi

printf '\nBundled native bridge:\n'
if [[ ! -f "$bridge_path" ]]; then
  printf '[WARN] Linux bridge not built: %s\n' "$bridge_path"
  warn=$((warn + 1))
else
  printf 'Bridge: %s\n' "$bridge_path"
  if file "$bridge_path" | grep -q 'ELF 64-bit'; then
    printf '[ OK ] bridge is a 64-bit ELF shared object\n'
    ok=$((ok + 1))
  else
    printf '[FAIL] bridge is not a 64-bit ELF shared object\n'
    fail=$((fail + 1))
  fi
  if ldd "$bridge_path" | grep -q 'not found'; then
    printf '[FAIL] bridge has unresolved DT_NEEDED libraries\n'
    ldd "$bridge_path"
    fail=$((fail + 1))
  else
    printf '[ OK ] all DT_NEEDED bridge libraries resolve\n'
    ok=$((ok + 1))
  fi
  if readelf -d "$bridge_path" 2>/dev/null | grep -qE 'RPATH|RUNPATH'; then
    printf '[WARN] bridge contains RPATH/RUNPATH; verify it is intentional\n'
    warn=$((warn + 1))
  else
    printf '[ OK ] bridge has no developer-machine RPATH/RUNPATH\n'
    ok=$((ok + 1))
  fi
  printf 'GLIBC baseline symbols: '
  objdump -T "$bridge_path" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n 1 || printf 'unknown\n'
  printf 'GLIBCXX baseline symbols: '
  objdump -T "$bridge_path" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -Vu | tail -n 1 || printf 'unknown\n'
fi

printf '\nResult: %d OK, %d warning(s), %d failure(s)\n' "${ok}" "${warn}" "${fail}"
if (( fail > 0 )); then
  printf 'The application/player cannot be considered ready on this system.\n'
  exit 1
fi
printf 'MPV/WebKit runtime prerequisites are present. Install libVLC to enable the VLC backend.\n'
exit 0
