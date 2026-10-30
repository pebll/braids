braids
======

braids is an open source desktop messenger for **Telegram, WhatsApp, Signal
and Matrix** in one app, for Linux and macOS.

> **Status: early development (milestone M0).** There is no usable app yet:
> this repository currently contains the protocol layer and a skeleton core
> daemon. See [doc/REQUIREMENTS.md](doc/REQUIREMENTS.md) and
> [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the plan.

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

Run the tests:

    ./make.sh tests

Requirements: CMake, a C++14 compiler, Go >= 1.23 (the Go toolchain pinned in
the protocol modules is downloaded automatically with `GOTOOLCHAIN=auto`),
OpenSSL, SQLite, zlib, libmagic, gperf. Signal support downloads a pre-built
libsignal, or builds it from source (Rust, protoc) if unavailable.

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
