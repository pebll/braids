Changelog
=========

What changed in each braids release. The newest version is at the top. The
release workflow publishes the section of a version as its release notes.

## [Unreleased]

### New
- Matrix groups: add people (they get an invitation) with "Add people" in
  the group info, remove them from a member's menu ("Remove from group").
  Both show only where your power level in the room allows it. (#38)
- F11 toggles fullscreen (a shortcut you can change in Settings →
  Keyboard). (#34)
- NixOS: a guide to installing braids with a system flake, in the README
  and linked from the website. (#37)

### Changed
- Sidebar: only All chats shows its unread count in color. The Unread
  folder has no count (it was always the same as All chats); Favorites and
  the accounts show theirs in grey, and a setting (Settings → General) hides
  them. Archived chats with new messages now also show their count in the
  collapsed sidebar. (#36)
- Settings → About → Report a bug fills in the braids version, how braids
  was installed, your distribution, kernel and desktop, and your
  messengers. (#39)

### Fixed
- In the image viewer, Ctrl + / Ctrl − / Ctrl 0 zoomed the whole app
  instead of the picture. (#31)

## [0.3.4] - 2026-11-10

### Fixed
- Nix: `nix run github:pebll/braids` stopped with "cannot write modified
  lock file": the flake's lock file was missing (#32).

### Changed
- Flathub: a manifest that builds braids from source, offline
  (`utils/dist/flathub`, built by the Flathub workflow), for the
  submission to Flathub.

## [0.3.3] - 2026-11-09

### New
- Nix and NixOS: `nix run github:pebll/braids` runs the latest release
  (or `nix profile install github:pebll/braids`).
- A new braids version is also announced as a desktop notification (once
  per version, with desktop notifications on), and is checked for while
  braids keeps running, not only at start. "Get it" opens the website's
  download section instead of the GitHub release page.

### Changed
- Poll results: the leading option's bar is full and the others relative
  to it, each option shows its number of votes and the pictures of up to
  five voters (+N for more; Telegram does not tell who voted). (#29)
- Chat list: photos, voice messages, videos and files in the last message
  show an icon; photos a small preview ("3 Photos" with up to three for
  photos sent together), voice messages their length ("Voice message
  (2:12)") instead of the file name. (#28)
- The Flatpak is now io.github.pebll.braids (was org.braids.app), as
  Flathub requires. Installing it adds a new app: to keep your accounts,
  quit braids, then move the data and remove the old one:
  `mv ~/.var/app/org.braids.app ~/.var/app/io.github.pebll.braids` and
  `flatpak uninstall org.braids.app`. Its software center page has
  screenshots and the release notes.

### Fixed
- WhatsApp: votes in polls reach the other phones. In chats where WhatsApp
  uses its new account ids (LIDs) instead of phone numbers, braids encrypted
  the vote for the phone number and the others could not read it.
- After a start or an update, names and profile pictures show at once: braids
  keeps the pictures it knows (`braids-avatars.json` in the account's profile)
  and loads the saved names before the messenger's contact sync; pictures are
  then checked for changes in the background (they were fetched again one by
  one, names waited for the connection).

## [0.3.2] - 2026-11-09

### New
- Polls on all messengers: see them with live results, vote (and take the
  vote back), make your own with the poll button next to the attach button
  (single or multiple choice, up to 12 options). Telegram allows polls in
  groups only. Votes are kept across restarts.
- Text size: Settings → General, or Ctrl+Plus / Ctrl+Minus / Ctrl+0 (also
  Ctrl+=). Contributed by @Pierre-Pln-Bys (#21, #7).
- Emoji shortcodes: type `:` and a few letters (`:thu`) for suggestions,
  Tab or Enter inserts. Contributed by @Pierre-Pln-Bys (#18, #5).
- "Copy image" in the menu of photos. Contributed by @Pierre-Pln-Bys
  (#22, #9).
- Settings → About: Report a bug, Suggest a feature, Known issues (GitHub).
- Image viewer: Show in chat, Copy image (also Ctrl+C) and Save as (also
  Ctrl+S) next to "Open with default app".
- Matrix invites can be accepted or declined in braids: an invited room
  shows "<name> invited you" and Join / Decline instead of the message box
  (it said to use another Matrix client, and its name started with
  "[Invite]").
- Select several messages ("Select" in the message menu, or Ctrl+click;
  Shift+click adds a range) to forward, copy or delete them together. (#25)
- Telegram: "<contact> joined Telegram" messages are left out, and the new
  chat they open stays out of the list until someone writes in it. Turn
  them back on per account in Settings → Accounts.

### Fixed
- Signal: photos, files and voice messages could not be sent at all ("Not
  sent", also when forwarding): Signal's server now wants the upload's size
  when asked for an upload slot. (#26)
- Signal voice messages play on iPhones: they are sent as AAC, as Signal's
  own apps record them (Signal on iPhone plays no Ogg). Needs an AAC encoder
  (gstreamer1.0-libav, recommended by the .deb); without one the Ogg file
  is sent as before.
- WhatsApp photos sent together (an album) are shown; they were dropped.
  Older WhatsApp photos and files, removed from WhatsApp's servers, are asked
  from the sender's phone again, as WhatsApp's apps do. (#24)
- Forwarding WhatsApp photos to Signal: the photo is downloaded (also older
  ones), and a Signal message whose picture cannot be uploaded is marked
  "Not sent" instead of going out without it. (#26)
- WhatsApp unread counts were too high after connecting: of the chat history
  WhatsApp sends, every received message counted as new when the chat had
  any unread. Now only as many as WhatsApp counts; the "New messages" line
  sits accordingly (after your last own or read message, not above the
  unread count).
- Links no longer open inside braids, with no way back (when no browser was
  running): the window never leaves braids, web pages go to the default
  browser. (#20)

### Changed
- An account that is not online has a bigger status dot in the sidebar, so a
  problem stands out.
- Archived chats are quiet like muted chats: no notifications, a grey
  unread count, and "Archived" in the sidebar shows how many of them have
  new messages (it counted all archived chats).
- Documentation formatting tidied up. Contributed by @Pierre-Pln-Bys (#19).

## [0.3.1] - 2026-11-05

### New
- Keyboard shortcuts overview: Ctrl+/ lists all shortcuts with your own
  bindings, and links to Settings → Keyboard to change them. Contributed by
  @Pierre-Pln-Bys (#14, #8).
- Profile pictures open in full size: the viewer shows the small picture at
  once and switches to the full-resolution one when it has been fetched
  (Telegram, WhatsApp and Matrix; Signal has one size).
- Test builds of pull requests: `install.sh --pr <number>` (or
  `--branch <name>`) installs them without a release; see the website's
  download section.

### Fixed
- The app icon reads at small sizes (GNOME top bar, browser tab): a bigger
  braid with thicker strands on a warm grey circle that stays visible on
  dark panels.
- Shortcuts follow the key's character on any layout: Ctrl+, opens Settings
  also where "," sits on the US M key (it went to Matrix). Non-latin layouts
  keep using the US letter positions.
- The tray icon could stay empty in GNOME's top bar after braids restarted:
  every start reused the same icon file name, which the panel kept cached.
  Each start now uses its own file, and leftovers are removed.
- Labels no longer break onto two lines in narrow controls ("On top" in
  Settings, "Message a contact" in New chat).

### Changed
- "Go to" shortcuts (folders, accounts, Ctrl+N with nothing unread, back to
  chat list) clear the search. The first chat gets a light highlight that
  turns full blue when you move it and fades back after 10 seconds; the
  mouse hover hides until the mouse moves.

## [0.3.0] - 2026-11-05

### New
- Messages you send show up at once. While an account is offline they wait,
  greyed out with a clock, and go out as soon as it is back (also after
  restarting braids). A message the messenger refuses is marked
  "Not sent", with "Send again" and "Delete". (#10)
- braids reconnects right after the computer wakes up or the network comes
  back, instead of waiting for the connection to time out. Each account in
  the sidebar shows its state with a dot (green online, amber connecting or
  fetching, grey offline, red error), and its tooltip says when it was
  last up to date. (#13)
- Keyboard: Alt+J/K (and the arrow keys) move a highlight through the chat
  list and the search results without opening anything. Enter opens the
  highlighted chat and puts the cursor in the message box. New shortcut
  "Back to chat list" (Alt+H or Alt+←). In a narrow window the list and
  chat shortcuts switch back to the list. A click still opens a chat
  directly. (#12)
- An "Unread" folder (Ctrl+U) with the unread chats.
- An unread chat opens at the "New messages" line, a read one at the
  newest message.
- Click a profile picture in the chat info to see it full size. In groups,
  a click on someone's picture or name (or on a member in the chat info)
  offers "View profile picture" and "Message privately".
- Ctrl+N with no unread chat left closes the open chat. (#11)

### Fixed
- Videos (MP4 and others), GIF videos and video stickers play in the app.
  Most MP4 videos also need GStreamer's libav plugin; the video viewer
  names the package to install, and so does the install script.
- Opening a chat with Enter from the search no longer puts an empty line
  in the message box.
- Ctrl+N clears the search and goes through all chats.
- Closing the search in a chat (Esc or ×) puts the cursor back in the
  message box.

## [0.2.0] - 2026-11-02

### New
- Search the messages of all chats from the chat list (Ctrl+Space). Results
  are highlighted, and opening one jumps to the message.
- Stickers and GIFs show inline, including Telegram's animated stickers.
- Previews for links in messages. They can be turned off in Settings.
- An emoji picker in the composer, with search and recently used emoji.
- A bar for pinned messages at the top of a chat.
- Create new groups on Telegram, WhatsApp, Signal and Matrix.
- Drag and drop files into a chat.
- Ctrl+C copies text: the selection, or the message under the mouse.
- Matrix: sign in with single sign-on (SSO and SAML) in the browser.
- Rename accounts from the sidebar, for example "Matrix" to "Work".
- Resize the chat list by dragging the divider. The sidebar has a collapse
  button.
- Favorites can be a folder, pinned at the top of the list, or both.
- Start braids at login (Settings), optionally minimized to the tray.
- braids tells you when a new version is available (Settings).
- New packages: AppImage, Flatpak and ARM64 (aarch64) builds.

### Fixed
- Only one braids window runs at a time. Starting it again brings the
  running window to the front.
- Voice messages and audio files (Opus, m4a) that the window could not play
  now play. If the system lacks the codec, they open in your audio player.
- Matrix: lists keep their `-` bullets after a message is edited.
- The selection color is visible in dark mode.
- The app icon shows up in GNOME.

## [0.1.0] - 2026-11-01

The first release.

- Telegram, WhatsApp, Signal and Matrix (with end-to-end encryption) in one
  desktop app for Linux.
- One chat list across all accounts, with folders for each messenger,
  favorites, muting, archive and "mark unread".
- Text with formatting, replies, reactions, @mentions, edit and delete.
- Photos and videos with a built-in viewer and player, files, and voice
  messages (play and record).
- Forward messages from any messenger to any other.
- Join groups by invite link. Group info, members and descriptions.
- Shared media for each chat, read receipts, typing indicators,
  notifications and an unread tray icon.
- Keyboard shortcuts for everything, all of which can be changed. Light and
  dark theme.
- Install script, .deb package and a self-contained archive.

[Unreleased]: https://github.com/pebll/braids/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/pebll/braids/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/pebll/braids/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/pebll/braids/releases/tag/v0.1.0
