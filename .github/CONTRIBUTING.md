Contributing
============
braids is in early development, see [doc/REQUIREMENTS.md](/doc/REQUIREMENTS.md)
and [doc/ARCHITECTURE.md](/doc/ARCHITECTURE.md) for scope and design.

- Open an issue (bug) or discussion (idea) before larger pull requests.
- Keep protocol libraries (`lib/*chat`) free of UI concerns; they talk to the
  core only through `lib/common/src/protocol.h`.
- Never log secrets (tokens, passwords, keys) or message plaintext.
- By contributing you agree that your contribution is licensed under the
  GNU AGPL v3 (see LICENSE).
