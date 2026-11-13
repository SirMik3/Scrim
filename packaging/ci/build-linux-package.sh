#!/usr/bin/env bash
# Usage (as root, inside the target distro): build-linux-package.sh scrim-VERSION.tar.gz OUTDIR
# Builds the native package for the running distro (Debian/Ubuntu .deb, Fedora/openSUSE
# .rpm, Arch .pkg.tar.zst) from a release tarball, then installs it and smoke-tests it.
set -euo pipefail
TARBALL=$(readlink -f "$1")
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
PKGDIR=$(cd "$(dirname "$0")/.." && pwd)          # packaging/
SRC=$(basename "$TARBALL" .tar.gz)            # scrim-VERSION
SCRIM_VERSION=${SRC#scrim-}
# Read os-release in a subshell: it defines NAME/VERSION, which must not leak into ours.
read -r OS_ID OS_VERSION_ID OS_CODENAME < <(. /etc/os-release; echo "$ID ${VERSION_ID:-rolling} ${VERSION_CODENAME:-unstable}")
WORK=$(mktemp -d)
chmod 755 "$WORK"                             # Arch builds run as an unprivileged user
log() { printf '\n==> %s\n' "$*"; }

smoke_test() {
    log "smoke test"
    scrim --help >/dev/null
    local list
    list=$(scrim --list)
    echo "$list"
    grep -q '^  rain ' <<<"$list" || { echo "rain effect missing"; exit 1; }
    if grep -q 'glass-rain' <<<"$list"; then echo "non-commercial effect was packaged"; exit 1; fi
    test -f /usr/lib/systemd/user/scrim.service
    echo "smoke test passed"
}

build_deb() {
    export DEBIAN_FRONTEND=noninteractive
    log "installing build dependencies"
    apt-get update -q
    apt-get install -y -q --no-install-recommends build-essential dpkg-dev debhelper fakeroot >/dev/null
    local deps
    deps=$(awk '/^Build-Depends:/ {f=1; sub(/^Build-Depends:/, ""); printf "%s", $0; next}
                f && /^[ \t]/   {printf "%s", $0; next} {f=0}' "$PKGDIR/debian/control")
    apt-get satisfy -y -q --no-install-recommends "$deps" >/dev/null

    log "building .deb for $OS_ID $OS_VERSION_ID"
    tar -xzf "$TARBALL" -C "$WORK"
    cd "$WORK/$SRC"
    cp -r "$PKGDIR/debian" debian
    cat > debian/changelog <<CHANGELOG
scrim ($SCRIM_VERSION-1~$OS_ID$OS_VERSION_ID) $OS_CODENAME; urgency=medium

  * Automated build of Scrim $SCRIM_VERSION.

 -- $(sed -n 's/^Maintainer: //p' debian/control)  $(date -R)
CHANGELOG
    dpkg-buildpackage -b -us -uc
    local deb
    deb=$(ls ../"scrim_${SCRIM_VERSION}-1~${OS_ID}${OS_VERSION_ID}"_*.deb)
    cp "$deb" "$OUT/"
    apt-get install -y -q "$OUT/$(basename "$deb")" >/dev/null
    smoke_test
}

build_rpm() {
    local install dist=()
    case "$OS_ID" in
        fedora) install="dnf install -y -q"; $install rpm-build >/dev/null ;;
        *)      install="zypper -n -q install"; $install rpm-build >/dev/null
                dist=(--define "dist .opensuse") ;;
    esac
    log "installing build dependencies"
    # shellcheck disable=SC2046
    $install $(rpmspec -q --buildrequires "$PKGDIR/rpm/scrim.spec") >/dev/null

    log "building .rpm for $OS_ID $OS_VERSION_ID"
    mkdir -p "$WORK"/{SOURCES,RPMS,BUILD,SPECS,SRPMS}
    cp "$TARBALL" "$WORK/SOURCES/"
    rpmbuild -bb --define "_topdir $WORK" --define "scrim_version $SCRIM_VERSION" "${dist[@]}" \
        "$PKGDIR/rpm/scrim.spec"
    local rpm
    rpm=$(find "$WORK/RPMS" -name 'scrim-[0-9]*.rpm' | head -1)
    cp "$rpm" "$OUT/"
    case "$OS_ID" in
        fedora) dnf install -y -q "$rpm" >/dev/null ;;
        *)      zypper -n -q install --allow-unsigned-rpm "$rpm" >/dev/null ;;
    esac
    smoke_test
}

build_arch() {
    log "installing build dependencies"
    # pacman 7 sandboxes downloads with landlock/seccomp, which containers (and
    # emulated ones especially) often can't provide. Fine to disable in a throwaway build.
    sed -i -e 's/^#\(DisableSandbox[A-Za-z]*\)/\1/' -e 's/^DownloadUser/#DownloadUser/' /etc/pacman.conf
    grep -q '^DisableSandboxSyscalls' /etc/pacman.conf ||
        sed -i '0,/^\[options\]/s//[options]\nDisableSandboxFilesystem\nDisableSandboxSyscalls/' /etc/pacman.conf
    pacman -Syu --noconfirm --needed base-devel >/dev/null
    mkdir -p "$WORK/pkg"
    cp "$PKGDIR/arch/PKGBUILD" "$TARBALL" "$WORK/pkg/"
    local sum
    sum=$(sha256sum "$TARBALL" | cut -d' ' -f1)
    sed -i -e "s/^pkgver=.*/pkgver=$SCRIM_VERSION/" -e "s/^sha256sums=.*/sha256sums=('$sum')/" "$WORK/pkg/PKGBUILD"
    # shellcheck disable=SC1091,SC2154  # depends/makedepends come from the PKGBUILD
    (source "$WORK/pkg/PKGBUILD"; pacman -S --noconfirm --needed "${depends[@]}" "${makedepends[@]}" >/dev/null)

    log "building package for Arch"
    id -u builder >/dev/null 2>&1 || useradd -m builder
    chown -R builder "$WORK/pkg"
    runuser -u builder -- bash -c "cd '$WORK/pkg' && makepkg --noconfirm --nodeps"
    local pkg
    pkg=$(find "$WORK/pkg" -maxdepth 1 -name "scrim-$SCRIM_VERSION-*.pkg.tar.zst" | head -1)
    cp "$pkg" "$OUT/"
    pacman -U --noconfirm "$pkg" >/dev/null
    smoke_test
}

case "$OS_ID" in
    debian|ubuntu)          build_deb ;;
    fedora|opensuse*)       build_rpm ;;
    arch)                   build_arch ;;
    *) echo "unsupported distro: $OS_ID" >&2; exit 1 ;;
esac
log "packages:"
ls -l "$OUT"
