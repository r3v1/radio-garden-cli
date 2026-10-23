# Radio Garden CLI

C++17 CLI/TUI client for [Radio Garden](https://radio.garden/). Builds a single
`radio` binary.

## Build & run

```sh
# dependencies (Arch Linux)
pacman -S nlohmann-json mpv libcurl ncurses systemd-libs

make            # produces ./radio
./radio         # TUI scoped to Spain, resumes last station
./radio --help  # flags: -h, -l, -p <place>, -s <name>, -t, -v
make clean      # rm ./radio
```

No CMake, no test suite, no CI, no linter/formatter scripts. `.clang-format` is
present (LLVM-based, 4-space indent, 80 cols) but no `clang-format` target in
`make`.

## Architecture

**Entrypoint:** `src/main.cpp`. All sources are compiled as a single translation
unit per object file — no libraries or subdirectories.

| Module | Role |
|---|---|
| `tui.cpp` | ncurses TUI, vim-like modes (Normal/Search/Command), live search on every keystroke. A `PlayerBridge` thread owns all `PlayerController` calls so the UI never waits on mpv |
| `player.cpp` | Singleton `PlayerController` → `MpvClient` (fork+Unix socket IPC) |
| `index.cpp` | `RadioIndex` — 4 background workers load/fetch the radio page of every place in the country; search is synchronous and in-memory (~0.1 ms). Never do disk/network I/O while holding `mutex_` |
| `mpris.cpp` | systemd `sd-bus` MPRIS D-Bus server (media keys) |
| `radio.cpp` | Radio Garden API JSON parsing and URL building |
| `requests.cpp` | libcurl HTTP with cached JSON writes |
| `state.cpp` | Playback persistence (`~/.config/radio-garden-cli/state.json`) |
| `paths.cpp` | XDG directory helpers |
| `search_utils.hpp` | Header-only: accent-folding `normalizeForSearch`, `QueryMatcher` (token substring scoring, per-word fuzzy fallback) |

**Data flow:**
1. Places list fetched from `radio.garden/api/ara/content/places` → cached at
   `places.json` in config dir (gitignored, all JSON is).
2. On TUI start, `RadioIndex` background worker fetches radio pages for every
   place in the selected country, caching each to
   `~/.cache/radio-garden-cli/places/<id>.json`.
3. Search reads from the in-memory corpus (no disk/HTTP on hot path). A radio
   is listed on many place pages, so the corpus keeps each radio once, while
   the per-place lists (used by the place view) keep the page as returned.
4. Background HTTP errors go to `~/.cache/radio-garden-cli/tui.log`, not the
   screen (stderr is redirected while the TUI runs).

## Key details

- **Playback:** mpv `--no-video --really-quiet` via fork/exec. IPC over Unix
  socket at `$XDG_RUNTIME_DIR/radio-garden-cli/mpv.sock`.
- **State:** `~/.config/radio-garden-cli/state.json`. Resumed on startup if
  valid (validated against cached places list).
- **MPRIS:** D-Bus name `org.mpris.MediaPlayer2.radio_garden_cli`. Quitting
  through MPRIS sets `g_mpris_quit` which exits the TUI loop.
- **Default country** is `"Spain"` in `main.cpp` and `tui.hpp`.
- **Version:** `v0.3.0` (hardcoded in `showVersion()`).
- **Radio URL format:** `https://radio.garden/api/ara/content/listen/<id>/channel.mp3?<timestamp>`
- **No package manager / build system beyond make.** No CI, no tests, no
  pre-commit hooks. No dependency manager — libraries are system-installed.
