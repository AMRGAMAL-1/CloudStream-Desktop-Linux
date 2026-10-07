#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd "$root_dir/.." && pwd)"
version="${APP_VERSION:-$(sed -n 's/^APP_VERSION=//p' "$repo_dir/gradle.properties" | head -n 1)}"
version="${version:-0.0.0}"
output_dir="$root_dir/build/outputs/linux"
work_dir="$root_dir/build/linux-packaging"
app_source="$root_dir/build/compose/binaries/main/app/CloudStream-Desktop"
requested="${1:---all}"

case "$(uname -m)" in
    x86_64|amd64) deb_arch=amd64; rpm_arch=x86_64 ;;
    aarch64|arm64) deb_arch=arm64; rpm_arch=aarch64 ;;
    *) echo "Unsupported Linux architecture: $(uname -m)" >&2; exit 2 ;;
esac

if [[ ! -x "$repo_dir/gradlew" && ! -f "$repo_dir/gradlew" ]]; then
    echo "Gradle wrapper not found: $repo_dir/gradlew" >&2
    exit 2
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "Building the Compose runtime image..."
    (cd "$repo_dir" && bash ./gradlew :desktop-app:createDistributable --no-daemon -PAPP_VERSION="$version")
fi

if [[ ! -x "$app_source/bin/CloudStream-Desktop" ]]; then
    echo "Compose runtime image was not produced: $app_source" >&2
    exit 1
fi

# Train an AppCDS archive from the exact bundled runtime and classpath. This
# measurably shortens cold-start (JVM + ~200-jar classpath + Compose/Skia init)
# on JVMs that ship a static CDS archive (e.g. Temurin in CI). The run is
# time-boxed and best-effort: on failure the packaged app simply falls back to
# -Xshare:auto without the app archive.
# Local packaging skips training by default so no GUI window flashes on the
# developer's desktop; set CLOUDSTREAM_TRAIN_CDS=1 to force it.
generate_cds_archive() {
    local image="$1"
    local runtime="$image/lib/runtime/bin/java"
    local appdir="$image/lib/app"
    local cfg="$appdir/CloudStream-Desktop.cfg"
    if [[ "${CLOUDSTREAM_TRAIN_CDS:-0}" != "1" && -z "${CI:-}" ]]; then
        echo "AppCDS training skipped (local packaging; set CLOUDSTREAM_TRAIN_CDS=1 to train)"
        return 0
    fi
    if [[ ! -x "$runtime" ]]; then
        # jpackage strips bin/java from the runtime image; train with the same
        # JDK that produced it (CI: Temurin 21 from setup-java, on PATH).
        runtime="$(command -v java || true)"
    fi
    [[ -x "$runtime" ]] || { echo "AppCDS training skipped (no java available)"; return 0; }
    [[ -f "$cfg" ]] || return 0

    local mainclass=""
    local classpath=""
    while IFS= read -r line; do
        case "$line" in
            app.mainclass=*) mainclass="${line#app.mainclass=}" ;;
            app.classpath=*) classpath="${classpath:+$classpath:}$appdir/${line#app.classpath=\$APPDIR/}" ;;
        esac
    done < "$cfg"
    [[ -n "$mainclass" && -n "$classpath" ]] || return 0

    local launch_cmd
    if command -v xvfb-run >/dev/null 2>&1 && [[ -z "${DISPLAY:-}" ]]; then
        launch_cmd=(xvfb-run -a "$runtime")
    else
        launch_cmd=("$runtime")
    fi
    echo "Training AppCDS archive: $appdir/app.jsa ..."
    timeout 20 "${launch_cmd[@]}" \
        -XX:ArchiveClassesAtExit="$appdir/app.jsa" \
        -Xshare:auto -cp "$classpath" "$mainclass" >/dev/null 2>&1 || true
    if [[ -f "$appdir/app.jsa" ]]; then
        echo "AppCDS archive ready ($(du -h "$appdir/app.jsa" | cut -f1))"
    else
        echo "AppCDS training failed (non-fatal; -Xshare:auto fallback)"
    fi
}
generate_cds_archive "$app_source"

rm -rf "$work_dir"
mkdir -p "$output_dir" "$work_dir" "$work_dir/common/opt/cloudstream" \
    "$work_dir/common/usr/bin" "$work_dir/common/usr/share/applications" \
    "$work_dir/common/usr/share/icons/hicolor/512x512/apps" \
    "$work_dir/common/usr/share/icons/hicolor/256x256/apps" \
    "$work_dir/common/usr/share/icons/hicolor/128x128/apps" \
    "$work_dir/common/usr/share/icons/hicolor/64x64/apps"
cp -a "$app_source" "$work_dir/common/opt/cloudstream/"
install -m 0755 "$root_dir/packaging/linux/cloudstream-launcher" "$work_dir/common/usr/bin/cloudstream"
install -m 0644 "$root_dir/packaging/linux/com.cloudstream.CloudStreamDesktop.desktop" \
    "$work_dir/common/usr/share/applications/"
# The launcher/menu icon is the RGBA (rounded) linux_icon.png. The palette-based
# app_icon.png has no alpha channel and renders as a square in DE launchers.
install -m 0644 "$root_dir/src/main/resources/linux_icon.png" \
    "$work_dir/common/usr/share/icons/hicolor/512x512/apps/com.cloudstream.CloudStreamDesktop.png"
install -m 0644 "$root_dir/src/main/resources/linux_icon.png" \
    "$work_dir/common/usr/share/icons/hicolor/256x256/apps/com.cloudstream.CloudStreamDesktop.png"
install -m 0644 "$root_dir/src/main/resources/linux_icon.png" \
    "$work_dir/common/usr/share/icons/hicolor/128x128/apps/com.cloudstream.CloudStreamDesktop.png"
install -m 0644 "$root_dir/src/main/resources/linux_icon.png" \
    "$work_dir/common/usr/share/icons/hicolor/64x64/apps/com.cloudstream.CloudStreamDesktop.png"

make_tar() {
    local portable="$work_dir/portable"
    rm -rf "$portable"
    mkdir -p "$portable"
    cp -a "$app_source" "$portable/"
    install -m 0755 "$root_dir/packaging/linux/AppRun" "$portable/cloudstream"
    cp "$root_dir/packaging/linux/com.cloudstream.CloudStreamDesktop.desktop" "$portable/"
    cp "$root_dir/src/main/resources/linux_icon.png" "$portable/com.cloudstream.CloudStreamDesktop.png"
    local archive="$output_dir/CloudStream-Desktop-${version}-linux-$(uname -m).tar.gz"
    tar -C "$portable" -czf "$archive" .
    echo "Created $archive"
}

make_deb() {
    command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb is required for DEB packaging" >&2; return 1; }
    local stage="$work_dir/deb"
    rm -rf "$stage"
    mkdir -p "$stage/DEBIAN"
    cp -a "$work_dir/common/." "$stage/"
    cat > "$stage/DEBIAN/control" <<EOF
Package: cloudstream-desktop
Version: $version
Section: video
Priority: optional
Architecture: $deb_arch
Maintainer: CloudStream maintainers
Description: CloudStream Desktop media client
 JVM desktop client with a native GTK/WebKitGTK player surface.
Depends: libc6 (>= 2.35), libstdc++6 (>= 12), libgtk-3-0 | libgtk-3-0t64, libwebkit2gtk-4.1-0 | libwebkit2gtk-4.1-0t64, libmpv2 | libmpv1, libx11-6, libgl1, libegl1
Recommends: xwayland, gstreamer1.0-libav
Suggests: vlc
EOF
    local archive="$output_dir/cloudstream-desktop_${version}_${deb_arch}.deb"
    dpkg-deb --root-owner-group --build "$stage" "$archive" >/dev/null
    echo "Created $archive"
}

make_rpm() {
    command -v rpmbuild >/dev/null 2>&1 || { echo "rpmbuild is required for RPM packaging" >&2; return 1; }
    local top="$work_dir/rpm"
    rm -rf "$top"
    mkdir -p "$top/BUILD" "$top/BUILDROOT" "$top/RPMS" "$top/SOURCES" "$top/SPECS" "$top/SRPMS" "$top/tmp" "$top/RPMDB"
    cp "$root_dir/packaging/linux/cloudstream.spec" "$top/SPECS/cloudstream.spec"
    rpmbuild -bb "$top/SPECS/cloudstream.spec" \
        --define "_topdir $top" \
        --define "app_version $version" \
        --define "stage_root $work_dir/common" \
        --define "_tmppath $top/tmp" \
        --define "_dbpath $top/RPMDB" \
        --define "_build_id_links none" >/dev/null
    local rpm
    rpm="$(find "$top/RPMS" -type f -name '*.rpm' -print -quit)"
    [[ -n "$rpm" ]] || { echo "rpmbuild did not produce an RPM" >&2; return 1; }
    cp "$rpm" "$output_dir/cloudstream-desktop-${version}-1.${rpm_arch}.rpm"
    echo "Created $output_dir/cloudstream-desktop-${version}-1.${rpm_arch}.rpm"
}

make_appimage() {
    command -v appimagetool >/dev/null 2>&1 || {
        echo "appimagetool is required for AppImage packaging; refusing to create a fake AppImage" >&2
        return 1
    }
    local appdir="$work_dir/CloudStream.AppDir"
    rm -rf "$appdir"
    mkdir -p "$appdir"
    cp -a "$work_dir/common/." "$appdir/"
    install -m 0755 "$root_dir/packaging/linux/AppRun" "$appdir/AppRun"
    cp "$root_dir/packaging/linux/com.cloudstream.CloudStreamDesktop.desktop" "$appdir/"
    cp "$root_dir/src/main/resources/linux_icon.png" "$appdir/com.cloudstream.CloudStreamDesktop.png"
    local archive="$output_dir/CloudStream-Desktop-${version}-linux-$(uname -m).AppImage"
    ARCH="$(uname -m)" appimagetool "$appdir" "$archive"
    echo "Created $archive"
}

case "$requested" in
    --all)
        make_tar
        make_deb
        make_rpm
        make_appimage
        bash "$root_dir/packaging/linux/validate-linux-bundle.sh" "$app_source"
        ;;
    --tar) make_tar ;;
    --deb) make_deb ;;
    --rpm) make_rpm ;;
    --appimage) make_appimage ;;
    --validate) bash "$root_dir/packaging/linux/validate-linux-bundle.sh" "$app_source" ;;
    *) echo "Usage: $0 [--all|--tar|--deb|--rpm|--appimage|--validate]" >&2; exit 2 ;;
esac
