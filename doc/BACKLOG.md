braids — Backlog
================

Agreed features not scheduled in a milestone yet. Newest first. The
plan to 1.0 is in [ROADMAP.md](ROADMAP.md).

- **Web version**: braids in the browser. The UI already runs in a browser
  (Vite dev bridge); a real web version needs braidsd on a server the user
  runs (it holds the sessions and keys), an authenticated WebSocket transport
  instead of the local socket, and TLS. A hosted service is not planned
  (it would hold everyone's message keys).
- **Flathub** (scheduled for 1.0-rc): the Flatpak (io.github.pebll.braids, metainfo and
  Flathub's linter ready) is attached to releases; Flathub builds open
  source apps from source only, without network: a manifest building tdlib,
  libsignal (Rust, now a prebuilt download), the Go protocols (vendored
  modules), braidsd, the Tauri shell (cargo sources) and the UI (npm
  sources, flatpak-builder-tools) is still needed. macOS is not
  planned (decided against); Windows and mobile are out of scope.
