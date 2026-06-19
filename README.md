# Radio Garden CLI

A CLI/TUI for [Radio Garden](https://radio.garden/). Made for educational purposes for learning C++.

It scrapes the Radio Garden API and parses JSON with [nlohmann/json](https://github.com/nlohmann/json). Playback uses [mpv](https://mpv.io/) with IPC control and MPRIS for desktop media keys.

## Dependencies

On Arch Linux:

```sh
pacman -S nlohmann-json mpv libcurl ncurses systemd-libs
```

## Building

```sh
make
```

## Usage

```sh
./radio --help
```

### Default (no arguments)

Launches the TUI scoped to **Spain**, resumes the last station if available (background playback), and registers an MPRIS player for system media keys.

### Examples

Play stations by place (classic CLI):

```sh
./radio -p "Madrid"
```

### Search

Search is **in-memory** and designed to feel instant (similar to `fzf` / `rg` filtering):

- On startup, all cached place radios are loaded into a search corpus (normalized titles once).
- Each keystroke triggers a debounced background search (~55 ms); no disk or HTTP on the hot path.
- Matching supports substring, prefix, subsequence (≥3 chars), and word boundaries (`Bilbao`, `Bila`, etc.).
- Towns without cached radios still appear via place-name search; opening them fetches radios on demand.

A background indexer keeps filling the cache for all places in the selected country. More cached places = more radio results in search.

`fzf` and `ripgrep` are not required at runtime; filtering in-process avoids spawning a subprocess on every keypress.

### TUI keys

| Key | Action |
|-----|--------|
| `j` / `k` | Move selection |
| `h` | Back |
| `l` / Enter | Select / play |
| `/` or type to search | Enter search mode (vim-like) |
| `Esc` | Leave search mode (keep filter) |
| `q` | Quit TUI (playback continues) |
| `Space` / `XF86AudioPlay` | Play / pause |
| `]` / `XF86AudioNext` | Next station |
| `[` / `XF86AudioPrev` | Previous station |

### Data locations

- Config: `~/.config/radio-garden-cli/state.json`
- Cache: `~/.cache/radio-garden-cli/` (places, radio index)
- mpv IPC socket: `$XDG_RUNTIME_DIR/radio-garden-cli/mpv.sock`

## Comments

I'm not a C++ developer, so I'm sure there are many things that can be improved. I'm open to suggestions and pull requests.
