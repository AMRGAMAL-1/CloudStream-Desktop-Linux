Name:           cloudstream-desktop
Version:        %{?app_version}%{!?app_version:0.0.0}
Release:        1%{?dist}
Summary:        CloudStream Desktop media client
License:        GPL-3.0-or-later
URL:            https://github.com/AMRGAMAL-1/CloudStream-Desktop-Linux
# The jpackage launcher and the JNI bridge are built on the release baseline
# (glibc 2.35, GCC 11+ / GLIBCXX >= 3.4.29). Express the ABI requirements via
# ELF capabilities and glibc version instead of distro-specific package names.
Requires:       glibc >= 2.35
Requires:       libstdc++.so.6()(64bit)
Requires:       libgtk-3.so.0()(64bit)
Requires:       libwebkit2gtk-4.1.so.0()(64bit)
Requires:       (libmpv.so.2()(64bit) or libmpv.so.1()(64bit))
Requires:       libX11.so.6()(64bit)
Requires:       libGL.so.1()(64bit)
Requires:       libEGL.so.1()(64bit)
Recommends:     libwayland-client.so.0()(64bit)
Recommends:     xorg-x11-server-Xwayland
BuildArch:      %{_arch}

%description
CloudStream Desktop — free media center for streaming and downloading
movies, TV shows and anime. Extension-based, ad-free, with watch tracking.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a %{stage_root}/. %{buildroot}/

%files
/opt/cloudstream/CloudStream-Desktop
/usr/bin/cloudstream
/usr/share/applications/com.cloudstream.CloudStreamDesktop.desktop
/usr/share/icons/hicolor/512x512/apps/com.cloudstream.CloudStreamDesktop.png
/usr/share/icons/hicolor/256x256/apps/com.cloudstream.CloudStreamDesktop.png
/usr/share/icons/hicolor/128x128/apps/com.cloudstream.CloudStreamDesktop.png
/usr/share/icons/hicolor/64x64/apps/com.cloudstream.CloudStreamDesktop.png

%changelog
* Wed Oct 07 2026 CloudStream maintainers <maintainers@example.invalid> - 0.0.0-1
- Initial Linux package.
