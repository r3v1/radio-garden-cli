#include "tui.hpp"

#include "index.hpp"
#include "mpris.hpp"
#include "player.hpp"
#include "state.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cctype>
#include <iostream>
#include <ncurses.h>
#include <set>
#include <string>
#include <vector>

#ifndef KEY_PLAY
#define KEY_PLAY 0x1c5
#endif
#ifndef KEY_PAUSE
#define KEY_PAUSE 0x1c6
#endif
#ifndef KEY_NEXT
#define KEY_NEXT 0x1c7
#endif
#ifndef KEY_PREVIOUS
#define KEY_PREVIOUS 0x1c8
#endif

enum class ResultKind { Radio, Place };

struct SearchResult {
    ResultKind kind;
    RadioEntry radio;
    Place place;
};

enum class ScreenKind { Search, PlaceRadios };

enum class TuiMode { Navigate, SearchInput };

struct ScreenState {
    ScreenKind kind;
    Place place;
    std::vector<RadioEntry> placeRadios;
};

static std::string truncateStr(const std::string &s, size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    if (maxLen <= 3) {
        return s.substr(0, maxLen);
    }
    return s.substr(0, maxLen - 3) + "...";
}

static void utf8PopBack(std::string &s) {
    if (s.empty()) {
        return;
    }
    size_t pos = s.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        pos--;
    }
    s.erase(pos);
}

static bool isMediaPlayKey(int ch) {
    return ch == ' ' || ch == 'p' || ch == 'P' || ch == KEY_PLAY || ch == 0x179;
}

static bool isMediaNextKey(int ch) {
    return ch == 'n' || ch == ']' || ch == KEY_NEXT || ch == 0x17a;
}

static bool isMediaPrevKey(int ch) {
    return ch == 'N' || ch == '[' || ch == KEY_PREVIOUS || ch == 0x17b;
}

static void handleMediaKey(int ch) {
    if (isMediaPlayKey(ch)) {
        PlayerController::instance().togglePause();
        auto entry = PlayerController::instance().currentEntry();
        if (!entry.id.empty()) {
            notifyMprisMetadata(entry.radio_title, entry.place_title,
                                !PlayerController::instance().isPaused());
        }
    } else if (isMediaNextKey(ch)) {
        PlayerController::instance().next();
        auto entry = PlayerController::instance().currentEntry();
        if (!entry.id.empty()) {
            notifyMprisMetadata(entry.radio_title, entry.place_title, true);
        }
    } else if (isMediaPrevKey(ch)) {
        PlayerController::instance().prev();
        auto entry = PlayerController::instance().currentEntry();
        if (!entry.id.empty()) {
            notifyMprisMetadata(entry.radio_title, entry.place_title, true);
        }
    }
}

static bool isMediaKey(int ch) {
    return isMediaPlayKey(ch) || isMediaNextKey(ch) || isMediaPrevKey(ch);
}

static bool isPrintableKey(int ch) {
    return ch >= 32 && ch < 127;
}

static bool isNavigateCommand(int ch) {
    return isMediaKey(ch) || ch == 'q' || ch == 'Q' || ch == 'h' || ch == 'H' ||
           ch == KEY_LEFT || ch == 'j' || ch == 'J' || ch == KEY_DOWN ||
           ch == 'k' || ch == 'K' || ch == KEY_UP || ch == 'l' ||
           ch == KEY_RIGHT || ch == '\n' || ch == '\r' || ch == KEY_ENTER ||
           ch == '/';
}

static void appendUtf8Input(int ch, std::string &searchQuery) {
    searchQuery += static_cast<char>(ch);
    int additional = 0;
    if ((ch & 0xE0) == 0xC0) {
        additional = 1;
    } else if ((ch & 0xF0) == 0xE0) {
        additional = 2;
    } else if ((ch & 0xF8) == 0xF0) {
        additional = 3;
    }
    for (int i = 0; i < additional; ++i) {
        int next = getch();
        if (next != ERR) {
            searchQuery += static_cast<char>(next);
        }
    }
}

static bool playRadioEntry(const RadioEntry &entry,
                           const std::vector<RadioEntry> &queue, int index) {
    if (!PlayerController::instance().playOk(entry, queue, index)) {
        return false;
    }
    notifyMprisMetadata(entry.radio_title, entry.place_title, true);
    return true;
}

static bool handleSelect(ScreenState &screen, std::vector<ScreenState> &stack,
                         const std::vector<SearchResult> &searchResults,
                         int selected, std::string &statusMessage, TuiMode &mode) {
    if (screen.kind == ScreenKind::Search) {
        if (searchResults.empty()) {
            return false;
        }
        const SearchResult &sr = searchResults[selected];
        if (sr.kind == ResultKind::Radio) {
            std::vector<RadioEntry> queue;
            for (const auto &result : searchResults) {
                if (result.kind == ResultKind::Radio) {
                    queue.push_back(result.radio);
                }
            }
            int qIndex = 0;
            for (size_t i = 0; i < queue.size(); ++i) {
                if (queue[i].id == sr.radio.id) {
                    qIndex = static_cast<int>(i);
                    break;
                }
            }
            if (queue.empty()) {
                queue = {sr.radio};
                qIndex = 0;
            }
            if (!playRadioEntry(sr.radio, queue, qIndex)) {
                statusMessage = "Error al reproducir";
            } else {
                statusMessage.clear();
            }
        } else {
            ScreenState nextScreen;
            nextScreen.kind = ScreenKind::PlaceRadios;
            nextScreen.place = sr.place;
            nextScreen.placeRadios = fetchPlaceRadios(
                sr.place.id, sr.place.title, sr.place.country);
            if (nextScreen.placeRadios.empty()) {
                statusMessage = "No se pudieron cargar emisoras";
                return false;
            }
            statusMessage.clear();
            stack.push_back(nextScreen);
            mode = TuiMode::Navigate;
        }
        return true;
    }

    if (screen.placeRadios.empty()) {
        return false;
    }
    if (!playRadioEntry(screen.placeRadios[selected], screen.placeRadios,
                        selected)) {
        statusMessage = "Error al reproducir";
    } else {
        statusMessage.clear();
    }
    return true;
}

static std::string trimSearch(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

static std::vector<SearchResult>
buildSearchResults(RadioIndex &index, const std::string &query) {
    std::vector<SearchResult> results;
    std::string q = trimSearch(query);
    if (q.empty()) {
        return results;
    }

    auto radios = index.getSearchResults();
    std::set<std::string> placesWithRadios;
    for (const auto &r : radios) {
        SearchResult sr;
        sr.kind = ResultKind::Radio;
        sr.radio = r;
        results.push_back(sr);
        placesWithRadios.insert(r.place_id);
    }

    for (const auto &p : index.searchPlaces(q)) {
        if (placesWithRadios.count(p.id)) {
            continue;
        }
        SearchResult sr;
        sr.kind = ResultKind::Place;
        sr.place = p;
        results.push_back(sr);
    }
    return results;
}

static std::vector<std::string>
formatPlaceRadios(const std::vector<RadioEntry> &radios) {
    std::vector<std::string> lines;
    for (const auto &r : radios) {
        lines.push_back(r.radio_title);
    }
    return lines;
}

void runTUI(const std::string &defaultCountry) {
    setlocale(LC_ALL, "");
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);

    RadioIndex index(defaultCountry);
    index.startBackgroundBuild();

    std::vector<ScreenState> stack;
    ScreenState root;
    root.kind = ScreenKind::Search;
    stack.push_back(root);

    std::string searchQuery;
    int selected = 0;
    int scrollOffset = 0;
    TuiMode mode = TuiMode::Navigate;
    bool running = true;

    std::vector<SearchResult> searchResults;
    std::vector<std::string> placeRadioLabels;
    std::string lastBuiltQuery;
    bool searchPending = false;
    std::string statusMessage;
    auto lastMetadataPoll = std::chrono::steady_clock::now();

    while (running) {
        if (g_mpris_quit) {
            running = false;
            break;
        }

        int maxY, maxX;
        getmaxyx(stdscr, maxY, maxX);

        ScreenState &screen = stack.back();

        if (screen.kind == ScreenKind::Search && searchPending &&
            index.hasFreshSearchResults(trimSearch(searchQuery))) {
            searchResults = buildSearchResults(index, searchQuery);
            lastBuiltQuery = searchQuery;
            searchPending = false;
            selected = 0;
            scrollOffset = 0;
            if (searchResults.empty() && !trimSearch(searchQuery).empty()) {
                statusMessage = "Sin resultados";
            } else {
                statusMessage.clear();
            }
        }

        if (screen.kind == ScreenKind::Search && !searchPending &&
            searchQuery != lastBuiltQuery) {
            index.requestSearch(trimSearch(searchQuery));
            searchPending = true;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now -
                                                             lastMetadataPoll)
                .count() >= 5) {
            lastMetadataPoll = now;
            if (PlayerController::instance().pollMetadata()) {
                auto entry = PlayerController::instance().currentEntry();
                std::string song =
                    PlayerController::instance().currentSong();
                std::string mprisTitle =
                    song.empty() ? entry.radio_title : song;
                std::string mprisArtist = song.empty()
                                              ? entry.place_title
                                              : entry.radio_title;
                notifyMprisMetadata(mprisTitle, mprisArtist,
                                    !PlayerController::instance().isPaused());
            }
        }

        if (screen.kind == ScreenKind::Search) {
            if (selected >= static_cast<int>(searchResults.size()) &&
                !searchResults.empty()) {
                selected = static_cast<int>(searchResults.size()) - 1;
            }
            if (selected < 0) {
                selected = 0;
            }
        } else {
            placeRadioLabels = formatPlaceRadios(screen.placeRadios);
            if (selected >= static_cast<int>(placeRadioLabels.size()) &&
                !placeRadioLabels.empty()) {
                selected = static_cast<int>(placeRadioLabels.size()) - 1;
            }
            if (selected < 0) {
                selected = 0;
            }
        }

        clear();

        std::string nowLine = "Radio Garden - " + defaultCountry;
        auto current = PlayerController::instance().currentEntry();
        if (!current.id.empty()) {
            std::string icon =
                PlayerController::instance().isPaused() ? "||" : ">";
            std::string song =
                PlayerController::instance().currentSong();
            if (!song.empty()) {
                nowLine = icon + " " + song + " · " +
                          current.radio_title + " - " +
                          current.place_title;
            } else {
                nowLine = icon + " " + current.radio_title + " - " +
                          current.place_title;
            }
        }
        mvprintw(0, 0, "%s", truncateStr(nowLine, maxX - 1).c_str());

        if (index.isBuilding()) {
            std::string progress = "Indexando: " +
                                   std::to_string(index.buildProgress()) + "/" +
                                   std::to_string(index.buildTotal()) + " (" +
                                   std::to_string(index.corpusSize()) +
                                   " emisoras)";
            mvprintw(1, 0, "%s", truncateStr(progress, maxX - 1).c_str());
        } else if (screen.kind == ScreenKind::PlaceRadios) {
            std::string header = "Emisoras en " + screen.place.title +
                                 "  (h: atras, l/Enter: reproducir, q: salir)";
            mvprintw(1, 0, "%s", truncateStr(header, maxX - 1).c_str());
        } else if (mode == TuiMode::SearchInput) {
            mvprintw(1, 0, "%s",
                     truncateStr("Modo busqueda (Esc: listo, Enter: confirmar)",
                                 maxX - 1)
                         .c_str());
        } else if (searchPending || index.isSearchPending()) {
            mvprintw(1, 0, "%s",
                     truncateStr("Buscando: " + searchQuery + "...", maxX - 1)
                         .c_str());
        } else if (!statusMessage.empty()) {
            mvprintw(1, 0, "%s", truncateStr(statusMessage, maxX - 1).c_str());
        } else if (screen.kind == ScreenKind::Search && searchQuery.empty()) {
            mvprintw(1, 0, "%s",
                     truncateStr("/ o letra: buscar  j/k: mover  l/Enter: ok",
                                 maxX - 1)
                         .c_str());
        }

        int listTop = index.isBuilding() ? 2 : 2;
        int listBottom = maxY - 3;
        int visibleRows = std::max(1, listBottom - listTop);

        int itemCount = 0;
        if (screen.kind == ScreenKind::Search) {
            itemCount = static_cast<int>(searchResults.size());
        } else {
            itemCount = static_cast<int>(placeRadioLabels.size());
        }

        if (selected < scrollOffset) {
            scrollOffset = selected;
        }
        if (selected >= scrollOffset + visibleRows) {
            scrollOffset = selected - visibleRows + 1;
        }

        if (itemCount == 0) {
            if (screen.kind == ScreenKind::Search && searchQuery.empty() &&
                mode == TuiMode::Navigate) {
                mvprintw(listTop + 1, 2,
                         "Pulsa / o escribe para buscar (ej: Bilbao).");
            } else {
                mvprintw(listTop + 1, 2, "Sin resultados. Prueba otro nombre.");
            }
        }

        for (int row = 0; row < visibleRows; ++row) {
            int idx = scrollOffset + row;
            if (idx >= itemCount) {
                break;
            }
            std::string line;
            if (screen.kind == ScreenKind::Search) {
                const SearchResult &sr = searchResults[idx];
                if (sr.kind == ResultKind::Radio) {
                    line = "[Radio] " + sr.radio.radio_title + " - " +
                           sr.radio.place_title;
                } else {
                    line = "[Pueblo] " + sr.place.title;
                }
            } else {
                line = screen.placeRadios[idx].radio_title;
            }

            if (idx == selected) {
                attron(A_REVERSE);
                mvprintw(listTop + row, 0, "> %s",
                         truncateStr(line, maxX - 3).c_str());
                attroff(A_REVERSE);
            } else {
                mvprintw(listTop + row, 0, "  %s",
                         truncateStr(line, maxX - 3).c_str());
            }
        }

        std::string searchLabel = "Buscar: " + searchQuery;
        if (mode == TuiMode::SearchInput && screen.kind == ScreenKind::Search) {
            searchLabel = "/" + searchQuery + "_";
        }
        mvprintw(maxY - 2, 0, "%s", truncateStr(searchLabel, maxX - 1).c_str());

        std::string helpLine;
        if (mode == TuiMode::SearchInput) {
            helpLine = "Esc: listo  Enter: confirmar  Backspace: borrar";
        } else {
            helpLine =
                "/: buscar  j/k: mover  h: atras  l/Enter: ok  Space: pausa  q: salir";
        }
        mvprintw(maxY - 1, 0, "%s", truncateStr(helpLine, maxX - 1).c_str());

        refresh();

        int ch = getch();
        if (ch == ERR) {
            continue;
        }

        if (mode != TuiMode::SearchInput && isMediaKey(ch)) {
            handleMediaKey(ch);
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            running = false;
            continue;
        }

        if (mode == TuiMode::SearchInput && screen.kind == ScreenKind::Search) {
            if (ch == 27) {
                mode = TuiMode::Navigate;
                continue;
            }
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                mode = TuiMode::Navigate;
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127) {
                utf8PopBack(searchQuery);
                lastBuiltQuery.clear();
                index.requestSearch(trimSearch(searchQuery));
                searchPending = true;
                selected = 0;
                scrollOffset = 0;
                continue;
            }
            if (isPrintableKey(ch)) {
                searchQuery += static_cast<char>(ch);
                lastBuiltQuery.clear();
                index.requestSearch(trimSearch(searchQuery));
                searchPending = true;
                selected = 0;
                scrollOffset = 0;
                continue;
            }
            if (ch >= 128) {
                appendUtf8Input(ch, searchQuery);
                lastBuiltQuery.clear();
                index.requestSearch(trimSearch(searchQuery));
                searchPending = true;
                selected = 0;
                scrollOffset = 0;
                continue;
            }
            continue;
        }

        if (ch == 'h' || ch == 'H' || ch == KEY_LEFT) {
            if (stack.size() > 1) {
                stack.pop_back();
                selected = 0;
                scrollOffset = 0;
                mode = TuiMode::Navigate;
            }
            continue;
        }

        if (ch == 'l' || ch == KEY_RIGHT || ch == '\n' || ch == '\r' ||
            ch == KEY_ENTER) {
            if (handleSelect(screen, stack, searchResults, selected, statusMessage,
                           mode)) {
                selected = 0;
                scrollOffset = 0;
            }
            continue;
        }

        if (ch == '/') {
            if (screen.kind == ScreenKind::Search) {
                mode = TuiMode::SearchInput;
            }
            continue;
        }

        if (screen.kind == ScreenKind::Search && !isNavigateCommand(ch) &&
            (isPrintableKey(ch) || ch >= 128)) {
            mode = TuiMode::SearchInput;
            lastBuiltQuery.clear();
            searchPending = true;
            selected = 0;
            scrollOffset = 0;
            if (isPrintableKey(ch)) {
                searchQuery += static_cast<char>(ch);
            } else {
                appendUtf8Input(ch, searchQuery);
            }
            index.requestSearch(trimSearch(searchQuery));
            continue;
        }

        switch (ch) {
        case KEY_UP:
        case 'k':
        case 'K':
            selected--;
            if (selected < 0) {
                selected = std::max(0, itemCount - 1);
            }
            break;
        case KEY_DOWN:
        case 'j':
        case 'J':
            selected++;
            if (selected >= itemCount) {
                selected = 0;
            }
            break;
        default:
            break;
        }
    }

    index.stopBackgroundBuild();
    endwin();
}
