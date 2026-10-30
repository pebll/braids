reference
=========

Source material from nchat, kept for porting and **not built**:

- `nchat-ui/src` — nchat's ncurses UI. `uimodel.cpp` (`UiModel::Impl`) contains
  the chat model (chat list, message windows, history paging, unread/read
  state, reactions, mentions, notifications) that milestone M1 ports into
  `src/braidsd`, see `doc/ARCHITECTURE.md`. `main.cpp` shows how profiles are
  loaded and protocols started.
- `nchat-ui/doc`, `nchat-ui/README.md` — nchat's user documentation (key
  bindings, config options), useful when designing the GUI equivalents.
- `nchat-ui/utils`, `nchat-ui/github-workflows` — nchat's release, packaging
  and install tooling, may be reused for braidsd packaging.

Delete this directory once M1 no longer needs it. The code here is MIT
licensed (nchat), see `LICENSE.nchat`.
