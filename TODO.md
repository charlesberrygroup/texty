# Texty — IDE Development Roadmap

A cross-platform terminal IDE written in C, built in phases.

---

## Phase 1 — Core Editor (MVP) ✅

- [x] Open/save files, insert/delete characters, newlines
- [x] Arrow keys, Home/End, Page Up/Down, Ctrl+Home/End
- [x] Line number gutter, status bar, scrolling, terminal resize

---

## Phase 2 — Editor Enhancements ✅

- [x] Undo / Redo (Ctrl+Z / Ctrl+Y)
- [x] Clipboard & Selection (Shift+Arrow, Ctrl+A/C/X/V)
- [x] Multiple Buffers with tab bar (Ctrl+N/O/W, Ctrl+]/\\)
- [x] Search & Replace (Ctrl+F, F3, Shift+F3, Ctrl+R)
- [x] Syntax Highlighting (C, C++, Python, JS/TS, Rust, Go, JSON, Markdown, Shell, Makefile)
- [x] Smart Editing (auto-indent, tab→spaces, bracket matching, auto-close, Ctrl+G, F2, F4)

---

## Phase 3 — IDE Features ✅

### File Explorer ✅
- [x] Side panel (Ctrl+B) — browse, open, create, rename, delete files

### Region Highlight ✅
- [x] Mark selected lines with a visible box (Ctrl+U)

### Git Integration ✅
- [x] Gutter markers (added/modified/deleted)
- [x] Git status panel (F9) — stage files with 's'
- [x] Git blame (Shift+F9) — per-line author + date
- [x] Inline diff view (F10) — phantom lines from HEAD
- [x] Stage hunks (F11) — stage individual diff hunks
- [x] Commit (F12) — prompt for message, commit staged changes

### Build System ✅
- [x] Run build command (F5) — defaults to `make`
- [x] Parse gcc/clang errors into a bottom panel
- [x] Jump to error location (Enter on error)
- [x] Configurable via texty.json (`build_command`)

### Navigation & Search ✅
- [x] Fuzzy file finder (Ctrl+P)
- [x] Recent files (Ctrl+E) — persisted across sessions
- [x] Go-to-symbol in file (F7)
- [x] Go-to-symbol in workspace (Ctrl+T)
- [x] Command palette (F8) — searchable list of all commands

### Configuration ✅
- [x] Color themes (F6) — Default Dark, Default Light, Monokai, Gruvbox Dark
- [x] Custom themes from ~/.config/texty/themes/*.theme
- [x] Theme background color support (default_fg/default_bg)
- [x] Theme preference in texty.json (`theme` key)

### Configuration (deferred)
- [ ] Config file (~/.config/texty/config.toml)
- [ ] Custom key bindings
- [ ] Plugin system (dynamic shared libraries)

---

## Phase 4 — Advanced IDE

### Language Server Protocol (LSP) ✅
- [x] LSP client infrastructure (start/stop language servers)
- [x] JSON parser for LSP protocol
- [x] Message framing (Content-Length)
- [x] Inline error / warning diagnostics (gutter + status bar)
- [x] Auto-completion with popup (Ctrl+Space)
- [x] Go-to-definition (F1)
- [x] Find all references (command palette)
- [x] Hover documentation popup (Ctrl+K)
- [x] Code formatting (command palette)
- [x] Rename symbol (command palette)
- [x] Signature help (command palette)

### Embedded Terminal
- [ ] Open terminal panel (Ctrl+`)
- [ ] Run shell commands without leaving the editor
- [ ] Toggle terminal panel height

### Split Panes (GUI only) ✅
- [x] Split editor horizontally / vertically (Ctrl+Shift+D / Ctrl+Shift+R)
- [x] Move focus between panes (Ctrl+Shift+] / Ctrl+Shift+[ or click)
- [x] Close pane (Ctrl+Shift+W)
- [x] Mouse scroll targets pane under cursor
- [ ] TUI split panes (use ncurses windows — previous stdscr approach failed)

---

## Backlog / Ideas

- Mouse support in TUI (click to position cursor, scroll wheel — already in GUI)
- Minimap (overview of file on the right side)
- Breadcrumb navigation (shows current function/class in status bar)
- Macro recording and playback
- Multi-cursor editing
- Column selection mode
- Code folding
- Snippets
- Session restore (reopen last files on startup)
- Remote file editing (SSH)
- Hex editor mode
- Diff viewer (compare two files side by side)
