# package.nix
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
#
# braids for Nix: the release tarball (utils/dist/package-linux) with its
# libraries taken from nixpkgs. The version and checksums are in
# release.json, updated for each release by utils/dist/nix-update.
#
#   nix run github:pebll/braids
#   nix profile install github:pebll/braids

{ lib
, stdenv
, fetchurl
, autoPatchelfHook
, wrapGAppsHook3
, webkitgtk_4_1
, gtk3
, glib
, glib-networking
, cairo
, gdk-pixbuf
, libsoup_3
, openssl
, sqlite
, file
, zlib
, libpng
, dbus
, libxcb
, gst_all_1
, libayatana-appindicator
, xdg-utils
, release ? lib.importJSON ./release.json
, tarball ? null # a local tarball instead of the release download (tests)
}:

let
  arch = stdenv.hostPlatform.parsed.cpu.name; # x86_64 or aarch64
in
stdenv.mkDerivation {
  pname = "braids";
  inherit (release) version;

  src =
    if tarball != null then tarball
    else fetchurl {
      url = "https://github.com/pebll/braids/releases/download/v${release.version}/braids-linux-${arch}.tar.gz";
      sha256 = release.sha256.${arch} or (throw "braids: no release for ${arch}");
    };

  sourceRoot = "braids";

  nativeBuildInputs = [ autoPatchelfHook wrapGAppsHook3 ];

  buildInputs = [
    webkitgtk_4_1
    gtk3
    glib
    glib-networking # https for the web view (link previews, pictures)
    cairo
    gdk-pixbuf
    libsoup_3
    openssl
    sqlite
    file # libmagic: file types of attachments
    zlib
    libpng
    dbus
    libxcb
    stdenv.cc.cc.lib
    # voice messages (recording, Ogg/Opus, AAC for Signal) and media playback
    gst_all_1.gstreamer
    gst_all_1.gst-plugins-base
    gst_all_1.gst-plugins-good
    gst_all_1.gst-plugins-bad
    gst_all_1.gst-libav
  ];

  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp -a bin lib share $out/
    rm -f $out/share/braids-install.sh
    substituteInPlace $out/share/applications/braids.desktop --replace-quiet "Exec=braids" "Exec=$out/bin/braids"
    runHook postInstall
  '';

  preFixup = ''
    gappsWrapperArgs+=(
      # the tray icon library is loaded at run time
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ libayatana-appindicator ]}
      # opening links and files
      --prefix PATH : ${lib.makeBinPath [ xdg-utils ]}
    )
  '';

  meta = {
    description = "Telegram, WhatsApp, Signal and Matrix in one desktop app";
    homepage = "https://pebll.github.io/braids/";
    changelog = "https://github.com/pebll/braids/blob/v${release.version}/CHANGELOG.md";
    license = lib.licenses.agpl3Only;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" "aarch64-linux" ];
    mainProgram = "braids";
  };
}
