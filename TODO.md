# Texty — IDE Development Roadmap

A cross-platform terminal IDE written in C, built in phases.

---

## Phase 1 — Core Editor (MVP) ✅ IN PROGRESS

The goal of Phase 1 is a working, usable text editor with clean architecture
that all future phases will build on.

### Text Editing
- [x] Open a file from the command line
- [x] Display file contents with line numbers
- [x] Insert characters at cursor position
- [x] Delete characters (Backspace and Delete key)
- [x] Insert newlines (Enter key)
- [x] Save file (Ctrl+S)
- [x] Quit editor (Ctrl+Q), with warning on unsaved changes

### Cursor Movement
- [x] Arrow keys (Up, Down, Left, Right)
- [x] Home / End — jump to start/end of line
- [x] Page Up / Page Down — scroll one screen at a time
- [x] Ctrl+Home / Ctrl+End — jump to start/end of file
- [x] Desired-column tracking (cursor snaps back when moving vertically)

### Display
- [x] Line number gutter
- [x] Status bar: filename, cursor position (line:col), modified indicator
- [x] Scrolling viewport (horizontal and vertical)
- [x] Terminal resize support

---

## Phase 2 — Editor Enhancements

### Undo / Redo
- [x] Undo last action (Ctrl+Z)
- [x] Redo (Ctrl+Y / Ctrl+Shift+Z)
- [x] Undo history limit (configurable)

### Clipboard & Selection
- [x] Visual selection mode (Shift+Arrow keys)
- [x] Copy selection (Ctrl+C)
- [x] Cut selection (Ctrl+X)
- [x] Paste (Ctrl+V)
- [x] Select all (Ctrl+A)

### Search & Replace
- [ ] Find text (Ctrl+F)
- [ ] Find next / previous (F3 / Shift+F3)
- [ ] Replace (Ctrl+H)
- [ ] Case-sensitive and regex toggle options

### Multiple Buffers (Tabs)
- [x] Open new file in a new buffer (Ctrl+O)
- [x] Create new empty buffer (Ctrl+N)
- [x] Switch between buffers (Ctrl+Right / Ctrl+Left)
- [x] Close current buffer (Ctrl+W)
- [x] Tab bar at the top showing open buffers

### Syntax Highlighting
- [ ] Language detection by file extension
- [ ] C / C++ highlighting (keywords, types, strings, comments, preprocessor)
- [ ] Python highlighting
- [ ] JavaScript / TypeScript highlighting
- [ ] Rust highlighting
- [ ] Go highlighting
- [ ] JSON highlighting
- [ ] Markdown highlighting
- [ ] Shell script highlighting
- [ ] Makefile highlighting

### Smart Editing
- [ ] Auto-indent on newline (matches current line's indentation)
- [ ] Tab key inserts spaces (configurable tab width)
- [ ] Show/hide whitespace characters
- [ ] Word-wrap toggle
- [ ] Jump to line (Ctrl+G)
- [ ] Bracket matching highlight
- [ ] Auto-close brackets and quotes

---

## Phase 3 — IDE Features

### File Explorer
- [ ] Side panel showing directory tree
- [ ] Toggle file explorer (Ctrl+B)
- [ ] Open file from tree with Enter
- [ ] Create / rename / delete files and directories
- [ ] Expand/collapse directories

### Split Panes
- [ ] Split editor horizontally (Ctrl+Shift+-)
- [ ] Split editor vertically (Ctrl+Shift+\)
- [ ] Move focus between panes (Ctrl+Shift+Arrow)
- [ ] Close pane

### Build System Integration
- [ ] Run build command (F5 / Ctrl+Shift+B)
- [ ] Parse compiler errors and warnings from build output
- [ ] Error panel showing build output
- [ ] Jump to error location from error panel (Enter on error)
- [ ] Highlight error lines in editor
- [ ] Configurable build command per project (texty.json)

### Embedded Terminal
- [ ] Open terminal panel (Ctrl+`)
- [ ] Run shell commands without leaving the editor
- [ ] Toggle terminal panel height

---

## Phase 4 — Advanced IDE

### Language Server Protocol (LSP)
- [ ] LSP client infrastructure (start/stop language servers)
- [ ] Auto-completion with popup (Tab to accept)
- [ ] Go-to-definition (F12)
- [ ] Go-to-declaration (Ctrl+F12)
- [ ] Find all references (Shift+F12)
- [ ] Hover documentation popup
- [ ] Inline error / warning diagnostics
- [ ] Code formatting (Ctrl+Shift+F)
- [ ] Rename symbol (F2)
- [ ] Signature help for function calls

### Git Integration
- [ ] Show modified/added/deleted lines in gutter
- [ ] Git status panel
- [ ] Inline diff view
- [ ] Stage hunks
- [ ] Commit from editor

### Navigation & Search
- [ ] Fuzzy file finder (Ctrl+P)
- [ ] Go-to-symbol in file (Ctrl+Shift+O)
- [ ] Go-to-symbol in workspace (Ctrl+T)
- [ ] Command palette (Ctrl+Shift+P)
- [ ] Recent files list

### Configuration & Extensibility
- [ ] Config file (~/.config/texty/config.toml)
- [ ] Custom key bindings
- [ ] Theme support (color schemes)
- [ ] Plugin system (dynamic shared libraries)
- [ ] Per-project settings (texty.json in project root)

---

## Backlog / Ideas

- Mouse support (click to position cursor, scroll wheel)
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
