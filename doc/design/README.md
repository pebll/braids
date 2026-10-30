braids — GUI design handoff
===========================

The visual design for the M3 Tauri app, made from
[DESIGN_BRIEF.md](../DESIGN_BRIEF.md). The source of truth is the design
canvas (owner access; ask the maintainer for a share link):
https://claude.ai/artifact/NcqJg7PBPMj7kT5VMPeG4z

Everything a coding agent needs is in this folder, so the canvas is optional.


Contents
--------

| Path | What |
|---|---|
| `tokens.css` | All design tokens as CSS custom properties (light on `:root, .t-light`, dark on `.t-dark`). Use these names directly in the app. |
| `screenshots/*.png` | Rendered screenshot of every artboard (1×). Start here. |
| `mockups/*.html` | The same artboards as standalone HTML/CSS. Open in a browser; inspect for exact spacing, sizes and colors. Static markup, not app code. |
| `icons/` | `braids-mark.svg` / `-dark.svg` (logo), `braids-linux-512.svg`, `braids-macos-1024.svg` (rounded square on the 1024 grid, for `icns`), `braids-tray-black.svg` / `-white.svg` (16 px monochrome tray). |
| `prototype/prototype.dc.html` | Source of the clickable prototype (needs the canvas runtime, so read it, don't open it). Its `renderVals()` is a good reference for UI state: folders, filtering, message grouping, reply/react/attach, info panel, theme switching. |

Artboards (each in `-light` and `-dark`): `Welcome`, `Wizard` (all
add-account steps), `Main-wide` / `Main-medium` / `Main-narrow`
(responsive layouts), `KitchenSink` (every message type and state from
brief 3.3), `ChatList-states`, `EmptyStates`, `Settings`, `Dialogs`
(confirmations, new chat, lightbox, banners, toasts, notifications,
tray/dock badge, composer states). Plus `Tokens` and `Logo`.


Key decisions
-------------

- **Look:** calm warm neutrals and one copper accent (`--accent`
  `#A84A1A` light / `#E58A55` dark). System font stack (`--font`), mono for
  code (`--mono`). Lucide-style 24 px outline icons, 1.75 stroke (the
  mockups inline the SVG paths; use `lucide` in the app). All text pairs
  meet WCAG AA in both themes.
- **Layout widths:** sidebar 232 (collapsed 60), chat list 320–330, info
  panel 300 as a column ≥1280 px; below that the info panel floats as an
  overlay sheet (right, 12 px inset, `--shadow-3`); <900 px is a single
  pane with a back button and the sidebar as a drawer.
- **Account identity:** the protocol badge is a rounded tile filled with
  the account color (`--acct-*`, 8 presets, white glyph ≥4.9:1), ringed
  with the surface color, at the avatar's bottom-right corner. The mockups
  use letter glyphs (T/W/S/M) as placeholders; **in the app, put the
  official protocol icons in the tile** (e.g. from the services' brand
  kits or `simple-icons`). Default colors: Telegram blue, WhatsApp green,
  Signal violet, Matrix teal.
- **Bubbles:** radius 16, the tail corner 6 on the sender side; consecutive
  messages from one sender get 2 px spacing and 6 px inner corners. Own =
  `--own-bubble` (accent-tinted, right), others = `--other-bubble`
  (neutral, left). Group sender names use `--sender-1..8`. A message that
  mentions me uses `--mention`.
- **Chat row:** 44 px avatar, name (650 weight when unread), time (accent
  when unread), preview (sender prefix in groups, "You:" for own,
  italic accent "typing…", red "Draft:"), count badge (grey when muted).
  Selection = `--selected` fill; keyboard cursor = 2 px inset `--focus`
  ring (distinct from selection).
- **Protocol differences:** unsupported actions are hidden; time-limited
  ones are disabled with a reason tooltip (see the `Dialogs` board: "Edit
  window has expired", "Delete for everyone" disabled with the limit).
- **Braid motif:** only in the logo, welcome, empty states and the syncing
  step (horizontal plait that slides; static under
  `prefers-reduced-motion`). Never in the chat itself.
- **WebKitGTK/WKWebView safe:** plain CSS only (flex/grid, box-shadow,
  SVG masks). No `backdrop-filter`, no Chromium-only features.
