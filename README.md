braids
======

braids is an open source desktop messenger for **Telegram, WhatsApp, Signal
and Matrix** in one app. Website: https://pebll.github.io/braids/

Install (Linux, x86_64; installs to your home folder, no root needed):

    curl -fsSL https://pebll.github.io/braids/install.sh | sh

A .deb package and a self-contained archive are attached to every
[release](https://github.com/pebll/braids/releases). macOS and Windows are
not available yet.

braids is based on [nchat](https://github.com/d99kris/nchat) by Kristofer
Berggren, a terminal messenger. braids keeps nchat's protocol implementations
(plus a new Matrix backend with end-to-end encryption) and replaces the
terminal UI with a GUI.

Building
--------

Dependencies (Linux via apt/dnf/pacman..., macOS via Homebrew):

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

    utils/dist/package-linux     dist/braids-linux-x86_64.tar.gz and a .deb
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

Disclaimer
----------

braids is not affiliated with Telegram, WhatsApp/Meta, Signal or the Matrix
Foundation. WhatsApp support uses an unofficial implementation; WhatsApp may
disable or suspend accounts that violate their Terms of Service. Use at your
own risk.

License
-------

braids is licensed under the GNU Affero General Public License v3, see
[LICENSE](LICENSE). It contains code from nchat, licensed under the MIT
license, see [LICENSE.nchat](LICENSE.nchat). Third-party components are
listed in the `THIRD_PARTY_LICENSES` files.
