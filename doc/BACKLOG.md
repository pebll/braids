braids — Backlog
================

Agreed features not scheduled in a milestone yet. Newest first.

Profile pictures
----------------

Avatars for contacts, groups and own accounts instead of initials (initials
stay the fallback). None of the protocol libraries fetch avatars yet (nchat
was a terminal app), so this needs work per protocol plus API and GUI:

| Protocol | Source | Effort |
|---|---|---|
| Matrix | room / user `avatar_url` (mxc), downloaded as thumbnail | small, testable with the local Synapse |
| Telegram | `chat.photo_` / `user.profile_photo_`, downloaded via tdlib | small–medium |
| WhatsApp | `GetProfilePictureInfo` per JID, then HTTP download | medium, extra network requests |
| Signal | encrypted profile avatar, decrypted with the profile key | hardest |

- Protocol interface: avatar path on `ChatInfo` / `ContactInfo` (or a
  `NewAvatarNotify`), files cached under the profile dir, refreshed when the
  protocol reports a change.
- API: `avatar` (local path) on Chat, Contact and Account objects, an
  `avatar.updated` event.
- GUI: `Avatar.svelte` shows the image (chat list, header, info panel,
  members, message groups), falls back to initials.
