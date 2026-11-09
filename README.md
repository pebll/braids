<p align="center"><img src="site/img/braids-icon.svg" width="72" alt=""></p>

<h1 align="center">braids</h1>

<p align="center"><b>Telegram, WhatsApp, Signal and Matrix in one Linux desktop app.</b><br>
One inbox, no server in between, open source.</p>

<p align="center">
  <a href="https://github.com/pebll/braids/releases/latest"><img src="https://img.shields.io/github/v/release/pebll/braids?label=release" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue" alt="License: AGPL-3.0"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20x86__64%20%7C%20ARM64-informational" alt="Platform: Linux x86_64 and ARM64">
  <a href="https://pebll.github.io/braids/"><img src="https://img.shields.io/badge/website-pebll.github.io%2Fbraids-orange" alt="Website"></a>
</p>

<p align="center"><img src="site/img/demo.gif" width="860" alt="braids: switching between messengers, replying, reacting, forwarding a Telegram message to WhatsApp and searching all chats"></p>

Install
-------

Linux, x86_64 or ARM64. Installs to your home folder, no root needed:

    curl -fsSL https://pebll.github.io/braids/install.sh | sh

Prefer a package? Every [release](https://github.com/pebll/braids/releases/latest)
has a **.deb**, a **Flatpak**, an **AppImage** and a self-contained archive.
Uninstall with `curl -fsSL https://pebll.github.io/braids/install.sh | sh -s -- --uninstall`.

Nix and NixOS: `nix run github:pebll/braids` (or `nix profile install
github:pebll/braids`; on NixOS add the flake's `packages.<system>.default`
to your system or home packages).

Why braids
----------

- **One inbox.** Every chat of every account in one list, newest first.
  Filter by messenger, keep favorites on top, mute and archive the same way
  everywhere.
- **No server in between.** braids talks to each messenger directly from
  your computer, like their own desktop apps do. There is no braids cloud and
  no bridge service: your accounts and messages stay on your machine.
- **A native client, not a bundle of web pages.** One app, one search across
  all chats, one set of keyboard shortcuts, and forwarding from any messenger
  to any other.
- **Keyboard first.** Jump between accounts, chats and unread messages
  without the mouse; every shortcut can be changed.
- **Calm.** Unread chats get a soft glow instead of red badges; tray icon,
  notifications and per-chat mute.

Features
--------

Text with formatting, replies, reactions, @mentions, edit and delete ·
photos, videos, files (drag and drop) · voice messages (play and record) ·
stickers and GIFs · link previews · emoji picker · search across all chats ·
pinned messages · new groups and joining by link · group info and members ·
read receipts and typing · shared media · light and dark theme.

Linking accounts: Telegram by QR code or phone number, WhatsApp and Signal by
QR code (as a linked device, like their desktop apps), Matrix with password
or single sign-on on any homeserver, with end-to-end encryption.

Good to know
------------

braids is an **unofficial** client and is not affiliated with Telegram,
WhatsApp/Meta, Signal or the Matrix Foundation.

- **Telegram** and **Matrix** officially welcome third-party clients.
- **WhatsApp** support uses an unofficial implementation
  ([whatsmeow](https://github.com/tulir/whatsmeow)). WhatsApp may restrict or
  ban accounts that use unofficial clients; this is rare for normal use, but
  possible. Use it at your own risk.
- **Signal** support uses [mautrix-signal](https://github.com/mautrix/signal)'s
  implementation and links braids as a secondary device. Signal does not
  support third-party clients officially.

braids stores sessions and the message cache in your home folder
(`~/.config/braids`, files readable only by you). It never sends anything to
anyone except the messengers themselves; link previews are fetched directly
from the linked site and can be turned off.

braids is based on [nchat](https://github.com/d99kris/nchat) by Kristofer
Berggren, a terminal messenger. braids keeps nchat's protocol implementations
(plus a new Matrix backend with end-to-end encryption) and replaces the
terminal UI with a GUI.

Contributing
------------

Bug reports, ideas and pull requests are welcome; see
[CONTRIBUTING.md](.github/CONTRIBUTING.md). Issues labelled
[good first issue](https://github.com/pebll/braids/labels/good%20first%20issue)
are a good place to start. What changed in each version is in
[CHANGELOG.md](CHANGELOG.md).

Building
--------

Dependencies (via apt/dnf/pacman...):

    ./make.sh deps

Build the core daemon and protocol libraries (all protocols enabled by
default; Telegram's tdlib takes long to compile, skip it with
`--no-telegram` for quicker local builds):

    ./make.sh build
    ./build/bin/braidsd --list-protocols

Telegram uses braids' API credentials when they are given at build time:
copy `.env.example` to `.env` (git-ignored) and fill in `TG_API_ID` and
`TG_API_HASH`, or set them as environment variables. Without them a build
falls back to nchat's credentials, fine for development only.

Run the tests:

    ./make.sh tests

Requirements: CMake, a C++14 compiler, Go >= 1.23 (the Go toolchain pinned in
the protocol modules is downloaded automatically with `GOTOOLCHAIN=auto`),
OpenSSL, SQLite, zlib, libmagic, gperf. Signal support downloads a pre-built
libsignal, or builds it from source (Rust, protoc) if unavailable.

Packaging and releases
----------------------

    utils/dist/package-linux     dist/braids-linux-<arch>.tar.gz, a .deb, an
                                 AppImage (--appimage, needs appimagetool)
    utils/dist/flatpak/          Flatpak manifest (made from the tarball)
    flake.nix, nix/              Nix package (the release tarball)
    utils/dist/install.sh        the install script (curl ... | sh)

Pushing a tag `vX.Y.Z` (matching the version in `app/src-tauri/tauri.conf.json`)
runs `.github/workflows/release.yml`, which builds the packages with braids'
Telegram API credentials (repository secrets `TG_API_ID`, `TG_API_HASH`) and
publishes the GitHub release that `install.sh` downloads from. The website in
`site/` is published to GitHub Pages by `.github/workflows/pages.yml`
(screenshots: `cd app && node scripts/site-screenshots.mjs`).

Repository layout
-----------------

    lib/common       protocol interface shared by core and protocols
    lib/ncutil       utilities, message cache (from nchat)
    lib/tgchat       Telegram (tdlib)
    lib/wmchat       WhatsApp (whatsmeow)
    lib/sgchat       Signal (mautrix-signal / libsignal)
    lib/mxchat       Matrix (mautrix-go, e2ee)
    lib/duchat       dummy protocol for development
    src/braidsd      core daemon
    doc              requirements, architecture, protocol notes
    reference        nchat's terminal UI, kept as source material, not built

License
-------

braids is licensed under the GNU Affero General Public License v3, see
[LICENSE](LICENSE). It contains code from nchat, licensed under the MIT
license, see [LICENSE.nchat](LICENSE.nchat). Third-party components are
listed in the `THIRD_PARTY_LICENSES` files.
