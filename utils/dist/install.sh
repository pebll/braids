#!/bin/sh

# install.sh
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
#
# Installs (or updates) braids for the current user, no root needed:
#
#   curl -fsSL https://raw.githubusercontent.com/pebll/braids/main/utils/dist/install.sh | sh
#
#   ~/.local/share/braids/          the app (bin/braids, bin/braidsd, lib/...)
#   ~/.local/bin/braids             command
#   ~/.local/share/applications/    menu entry
#
# Options (after "sh -s --" when piped):
#   --uninstall   remove braids (keeps accounts and messages in ~/.config/braids)
#   --purge       remove braids and ~/.config/braids
#
# Environment:
#   BRAIDS_TARBALL   install from this local braids-linux-x86_64.tar.gz instead
#                    of downloading the latest release
#   BRAIDS_VERSION   a release tag (e.g. v0.2.0) instead of the latest

set -eu

REPO="pebll/braids"
DATA="${XDG_DATA_HOME:-${HOME}/.local/share}"
DEST="${DATA}/braids"
BIN="${HOME}/.local/bin"
APPS="${DATA}/applications"
ICONS="${DATA}/icons/hicolor"

say() { printf '%s\n' "$*"; }
die() { printf 'braids install: %s\n' "$*" >&2; exit 1; }

uninstall() {
  rm -rf "${DEST}"
  rm -f "${BIN}/braids" "${APPS}/braids.desktop" "${ICONS}/128x128/apps/braids.png" "${ICONS}/256x256/apps/braids.png"
  say "braids removed."
}

case "${1:-}" in
  --uninstall) uninstall; say "Accounts and messages are kept in ~/.config/braids."; exit 0 ;;
  --purge) uninstall; rm -rf "${XDG_CONFIG_HOME:-${HOME}/.config}/braids"; say "Accounts and messages removed."; exit 0 ;;
  "") ;;
  *) die "unknown option ${1} (use --uninstall or --purge)" ;;
esac

[ "$(uname -s)" = "Linux" ] || die "braids is available for Linux only for now."
[ "$(uname -m)" = "x86_64" ] || die "braids is available for x86_64 only for now (this is $(uname -m))."

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT INT TERM

if [ -n "${BRAIDS_TARBALL:-}" ]; then
  cp "${BRAIDS_TARBALL}" "${TMP}/braids.tar.gz"
else
  command -v curl >/dev/null 2>&1 || die "curl is needed."
  if [ -n "${BRAIDS_VERSION:-}" ]; then
    URL="https://github.com/${REPO}/releases/download/${BRAIDS_VERSION}/braids-linux-x86_64.tar.gz"
  else
    URL="https://github.com/${REPO}/releases/latest/download/braids-linux-x86_64.tar.gz"
  fi
  say "Downloading ${URL}"
  curl -fL --progress-bar -o "${TMP}/braids.tar.gz" "${URL}" || die "download failed."
  curl -fsSL -o "${TMP}/braids.tar.gz.sha256" "${URL}.sha256" || die "checksum download failed."
  EXPECTED="$(cut -d ' ' -f 1 "${TMP}/braids.tar.gz.sha256")"
  ACTUAL="$(sha256sum "${TMP}/braids.tar.gz" | cut -d ' ' -f 1)"
  [ "${EXPECTED}" = "${ACTUAL}" ] || die "checksum mismatch, not installing."
fi

tar -C "${TMP}" -xzf "${TMP}/braids.tar.gz" || die "could not unpack the download."
[ -x "${TMP}/braids/bin/braids" ] || die "unexpected archive contents."

# replace the previous version; a running braidsd is restarted by the app
# when it notices the newer binary
mkdir -p "${DATA}" "${BIN}" "${APPS}" "${ICONS}/128x128/apps" "${ICONS}/256x256/apps"
rm -rf "${DEST}.new"
mv "${TMP}/braids" "${DEST}.new"
rm -rf "${DEST}"
mv "${DEST}.new" "${DEST}"

ln -sf "${DEST}/bin/braids" "${BIN}/braids"
sed "s|^Exec=.*|Exec=${DEST}/bin/braids|" "${DEST}/share/applications/braids.desktop" > "${APPS}/braids.desktop"
cp "${DEST}/share/icons/hicolor/128x128/apps/braids.png" "${ICONS}/128x128/apps/braids.png"
cp "${DEST}/share/icons/hicolor/256x256/apps/braids.png" "${ICONS}/256x256/apps/braids.png"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "${APPS}" >/dev/null 2>&1 || true

say "braids $(cat "${DEST}/share/braids-version" 2>/dev/null || true) installed to ${DEST}"

# system libraries the app needs (the rest ships with braids)
MISSING=""
for lib in libwebkit2gtk-4.1.so.0 libgtk-3.so.0 libsqlite3.so.0 libmagic.so.1; do
  ldconfig -p 2>/dev/null | grep -q "${lib}" || MISSING="${MISSING} ${lib}"
done
if ! ldconfig -p 2>/dev/null | grep -q -e libayatana-appindicator3.so.1 -e libappindicator3.so.1; then
  MISSING="${MISSING} libayatana-appindicator3.so.1"
fi
if [ -n "${MISSING}" ]; then
  say ""
  say "Missing system libraries:${MISSING}"
  say "Install them with your package manager, e.g.:"
  say "  Debian/Ubuntu: sudo apt install libwebkit2gtk-4.1-0 libayatana-appindicator3-1 libsqlite3-0 libmagic1"
  say "  Fedora:        sudo dnf install webkit2gtk4.1 libayatana-appindicator-gtk3 sqlite-libs file-libs"
  say "  Arch:          sudo pacman -S webkit2gtk-4.1 libayatana-appindicator sqlite file"
fi

case ":${PATH}:" in
  *":${BIN}:"*) say "Start it from your app menu or with: braids" ;;
  *) say "Start it from your app menu or with: ${BIN}/braids  (${BIN} is not in your PATH)" ;;
esac
