# Builds on Fedora and openSUSE. Version comes from the release tarball:
#   rpmbuild -ba scrim.spec --define "scrim_version 0.1.0"
%{!?scrim_version: %global scrim_version 0.1.0}

Name:           scrim
Version:        %{scrim_version}
Release:        1%{?dist}
Summary:        Lightweight animated effects over your desktop wallpaper
License:        MIT
URL:            https://github.com/SirMik3/Scrim
Source0:        %{url}/releases/download/v%{version}/scrim-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(xfixes)
BuildRequires:  pkgconfig(xext)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(json-c)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libjpeg)
%if 0%{?suse_version}
# openSUSE ships the GLES3 headers separately from pkgconfig(glesv2)
BuildRequires:  Mesa-libGLESv3-devel
%endif

%description
Scrim draws GPU shader effects, such as rain on glass, in a click-through layer
between the wallpaper and your windows. It uses wlr-layer-shell on Wayland
(KDE Plasma, Sway, Hyprland and others) and a desktop-type window on X11, and
stops rendering when the desktop is hidden or the machine runs on battery.

%prep
%autosetup -n scrim-%{version}

%build
export CFLAGS="%{optflags}" LDFLAGS="%{?build_ldflags}"
%make_build -C linux PREFIX=%{_prefix}

%install
%make_install -C linux PREFIX=%{_prefix} RELEASE=1

%files
%license LICENSE
%doc README.md
%{_bindir}/scrim
%{_prefix}/lib/systemd/user/scrim.service
%{_datadir}/scrim/
