braids — Backlog
================

Agreed features not scheduled in a milestone yet. Newest first.

- **Sync mute and "mark unread" with the messengers**: both are braids-only
  for now (Telegram, WhatsApp and Matrix have APIs for them, Signal partly).
- **Web version**: braids in the browser. The UI already runs in a browser
  (Vite dev bridge); a real web version needs braidsd on a server the user
  runs (it holds the sessions and keys), an authenticated WebSocket transport
  instead of the local socket, and TLS. A hosted service is not planned
  (it would hold everyone's message keys).
- **Flathub**: the Flatpak (io.github.pebll.braids, metainfo and
  Flathub's linter ready) is attached to releases; Flathub builds open
  source apps from source only, without network: a manifest building tdlib,
  libsignal (Rust, now a prebuilt download), the Go protocols (vendored
  modules), braidsd, the Tauri shell (cargo sources) and the UI (npm
  sources, flatpak-builder-tools) is still needed. macOS is not
  planned (decided against); Windows and mobile are out of scope.

- **Custom groups**: user-defined chat groups/folders beyond All, Favorites
  and Archived.
- **Custom muting rules**: notification rules instead of only per-chat mute,
  e.g. mute personal chats during work hours, notify only for favorites,
  don't notify for groups.
- **Join Signal groups by invite link**: signalmeow has no API for it
  (needs the group join flow with the invite password).
- **Vim mode**: postponed; plan proposed (LIST / NORMAL / INSERT modes,
  doc/REQUIREMENTS.md §4), open questions on folder switching, Esc in
  INSERT, count prefixes and indicator position.
