# braids

One desktop app for the user's Telegram, WhatsApp, Signal and Matrix chats.
This glossary holds the words the app, its docs and its code use for what
the user sees.

## Language

**Messenger**:
One of the chat networks braids connects to: Telegram, WhatsApp, Signal or Matrix.
_Avoid_: protocol, service, network (in user-facing text; "protocol" is fine in core code)

**Account**:
The user's login on one messenger; there can be several per messenger.
_Avoid_: profile, login, connection

**Folder**:
A named subset of the chat list in the sidebar (All chats, Unread, Favorites, Archived, one per account).
_Avoid_: filter, tab, group

**Tips**:
A short list of features worth knowing, shown one at a time over the first days and always available in Settings.
_Avoid_: tour, tutorial, onboarding, hints

**Recovery key**:
The Matrix secret that unlocks older encrypted messages on a new device and marks it as trusted.
_Avoid_: security key, backup key, passphrase

**Device verification**:
Confirming a new braids device from another signed-in device by comparing emojis.
_Avoid_: cross-signing, session verification
