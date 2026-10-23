#include "tui.hpp"

#include "index.hpp"
#include "mpris.hpp"
#include "paths.hpp"
#include "player.hpp"
#include "search_utils.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <ncurses.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxRadioResults = 500;
constexpr size_t kMaxPlaceResults = 40;
// Place hits scoring below this (exact, prefix or word match) are listed
// before the radio results; the rest go after them. Scores from 500 up are
// fuzzy matches, only worth showing when no radio matched.
constexpr int kStrongPlaceScore = 40;
constexpr int kFuzzyScore = 500;
constexpr int kScrollOff = 3;
constexpr int kPollMs = 50;

// ---------------------------------------------------------------------------
// Text helpers (UTF-8, display width)
// ---------------------------------------------------------------------------

uint32_t decodeUtf8(const std::string &s, size_t &i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (len == 1 || i + len > s.size()) {
        i++;
        return c < 0x80 ? c : 0xFFFD;
    }
    uint32_t cp = c & (0xFFu >> (len + 1));
    for (size_t k = 1; k < len; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += len;
    return cp;
}

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int cellWidth(uint32_t cp) {
    int w = wcwidth(static_cast<wchar_t>(cp));
    return w < 0 ? 1 : w;
}

int textWidth(const std::string &s) {
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        width += cellWidth(decodeUtf8(s, i));
    }
    return width;
}

// Cuts `s` to at most `cols` display cells, ending in "…" when truncated.
std::string fitWidth(const std::string &s, int cols) {
    if (cols <= 0) {
        return "";
    }
    if (textWidth(s) <= cols) {
        return s;
    }
    std::string out;
    int used = 0;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        int w = cellWidth(decodeUtf8(s, i));
        if (used + w > cols - 1) {
            break;
        }
        out.append(s, start, i - start);
        used += w;
    }
    return out + "…";
}

std::string trim(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && search_detail::isAsciiSpace(s[b])) {
        b++;
    }
    while (e > b && search_detail::isAsciiSpace(s[e - 1])) {
        e--;
    }
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

struct Key {
    bool code = false; // function key (KEY_*) rather than a character
    wint_t c = 0;

    bool is(wchar_t ch) const { return !code && c == static_cast<wint_t>(ch); }
    bool isCode(int k) const { return code && c == static_cast<wint_t>(k); }
    bool ctrl(char letter) const { return is(letter & 0x1f); }
    bool esc() const { return is(27); }
    bool enter() const { return is('\n') || is('\r') || isCode(KEY_ENTER); }
    bool backspace() const { return is(127) || is(8) || isCode(KEY_BACKSPACE); }
    bool printable() const {
        return !code && c >= 32 && c != 127 && !(c >= 0x80 && c < 0xA0);
    }
};

// Single-line editor with a cursor, used by the search and command prompts.
struct LineEdit {
    std::wstring text;
    size_t cur = 0;

    void set(const std::string &utf8) {
        text.clear();
        for (size_t i = 0; i < utf8.size();) {
            text += static_cast<wchar_t>(decodeUtf8(utf8, i));
        }
        cur = text.size();
    }
    void clear() {
        text.clear();
        cur = 0;
    }
    std::string utf8Prefix(size_t n) const {
        std::string out;
        for (size_t i = 0; i < n && i < text.size(); ++i) {
            appendUtf8(out, static_cast<uint32_t>(text[i]));
        }
        return out;
    }
    std::string utf8() const { return utf8Prefix(text.size()); }

    // Applies an editing key. Returns true if the key was an editing key.
    bool apply(const Key &k) {
        if (k.printable()) {
            text.insert(cur, 1, static_cast<wchar_t>(k.c));
            cur++;
        } else if (k.backspace()) {
            if (cur > 0) {
                text.erase(--cur, 1);
            }
        } else if (k.isCode(KEY_DC)) {
            if (cur < text.size()) {
                text.erase(cur, 1);
            }
        } else if (k.isCode(KEY_LEFT)) {
            cur = cur > 0 ? cur - 1 : 0;
        } else if (k.isCode(KEY_RIGHT)) {
            cur = std::min(cur + 1, text.size());
        } else if (k.isCode(KEY_HOME) || k.ctrl('a')) {
            cur = 0;
        } else if (k.isCode(KEY_END) || k.ctrl('e')) {
            cur = text.size();
        } else if (k.ctrl('w')) {
            size_t end = cur;
            while (cur > 0 && std::iswspace(text[cur - 1])) {
                cur--;
            }
            while (cur > 0 && !std::iswspace(text[cur - 1])) {
                cur--;
            }
            text.erase(cur, end - cur);
        } else if (k.ctrl('u')) {
            text.erase(0, cur);
            cur = 0;
        } else if (k.ctrl('k')) {
            text.erase(cur);
        } else {
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Player bridge: every call into mpv happens on this thread, so the UI never
// waits for the socket. The UI only reads the cached snapshot.
// ---------------------------------------------------------------------------

class PlayerBridge {
  public:
    struct Snapshot {
        RadioEntry entry;
        std::string song;
        bool active = false;
        bool paused = false;
    };

    PlayerBridge() { thread_ = std::thread(&PlayerBridge::run, this); }

    ~PlayerBridge() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void play(const RadioEntry &entry, std::vector<RadioEntry> queue,
              int index) {
        post([this, entry, queue = std::move(queue), index] {
            if (!PlayerController::instance().playOk(entry, queue, index)) {
                setError("Error al reproducir " + entry.radio_title);
            }
        });
    }
    void togglePause() {
        post([] { PlayerController::instance().togglePause(); });
    }
    void next() {
        post([] { PlayerController::instance().next(); });
    }
    void prev() {
        post([] { PlayerController::instance().prev(); });
    }
    void stop() {
        post([] { PlayerController::instance().stop(); });
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }
    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
    std::string takeError() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string err;
        err.swap(error_);
        return err;
    }

  private:
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void setError(const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    }

    void run() {
        auto lastSongPoll = Clock::now();
        for (;;) {
            std::deque<std::function<void()>> tasks;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(500),
                             [&] { return stop_ || !tasks_.empty(); });
                if (stop_) {
                    return;
                }
                tasks.swap(tasks_);
            }
            bool ranTasks = !tasks.empty();
            for (auto &task : tasks) {
                task();
            }

            auto &player = PlayerController::instance();
            bool songChanged = false;
            auto now = Clock::now();
            if (ranTasks || now - lastSongPoll >= std::chrono::seconds(5)) {
                lastSongPoll = now;
                songChanged = player.pollMetadata();
            }

            Snapshot fresh;
            fresh.active = player.hasActivePlayback();
            fresh.entry = player.currentEntry();
            fresh.song = player.currentSong();
            fresh.paused = fresh.active && player.isPaused();

            bool changed;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                changed = fresh.entry.id != snapshot_.entry.id ||
                          fresh.song != snapshot_.song ||
                          fresh.active != snapshot_.active ||
                          fresh.paused != snapshot_.paused;
                if (changed) {
                    snapshot_ = fresh;
                    generation_++;
                }
            }
            if ((changed || songChanged) && !fresh.entry.id.empty()) {
                bool hasSong = !fresh.song.empty();
                notifyMprisMetadata(
                    hasSong ? fresh.song : fresh.entry.radio_title,
                    hasSong ? fresh.entry.radio_title : fresh.entry.place_title,
                    !fresh.paused);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    Snapshot snapshot_;
    uint64_t generation_ = 0;
    std::string error_;
    bool stop_ = false;
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

enum class ItemKind { Radio, Place };

struct Item {
    ItemKind kind = ItemKind::Radio;
    RadioEntry radio;
    Place place;
    size_t count = 0; // radios in the place (places only)
    std::string norm; // normalized radio title, for filtering inside a place

    std::string key() const {
        return kind == ItemKind::Radio ? "r:" + radio.id : "p:" + place.id;
    }
};

struct View {
    bool root = true;
    Place place;              // set when !root
    std::vector<Item> source; // !root: every radio of the place
    std::vector<Item> items;  // what is currently listed
    std::string query;
    int sel = 0;
    int top = 0;
};

enum class Mode { Normal, Search, Command };

struct Style {
    attr_t place = A_BOLD;
    attr_t playing = A_BOLD;
    attr_t dim = A_DIM;
};

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------

class App {
  public:
    App(const std::string &country, std::unique_ptr<RadioIndex> index)
        : country_(country), index_(std::move(index)) {
        setupStyle();
        views_.emplace_back();
        rebuild(views_.back(), false);
        lastVersion_ = index_->version();
        lastProgress_ = index_->buildProgress();
        lastBuilding_ = true;
        index_->startBackgroundBuild();
    }

    void run() {
        while (running_ && !g_mpris_quit) {
            pollBackground();
            if (dirty_) {
                render();
                dirty_ = false;
            }

            wint_t wch = 0;
            int rc = get_wch(&wch);
            if (rc == ERR) {
                continue;
            }
            // Handle everything already typed before redrawing once, so a
            // burst of keys (or a paste) never lags behind the screen.
            do {
                Key key;
                key.code = rc == KEY_CODE_YES;
                key.c = wch;
                handleKey(key);
                dirty_ = true;
            } while (running_ && readPending(rc, wch));
        }
    }

  private:
    // ----- setup -----------------------------------------------------------

    void setupStyle() {
        if (!has_colors()) {
            return;
        }
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
        style_.place = COLOR_PAIR(1) | A_BOLD;
        style_.playing = COLOR_PAIR(2) | A_BOLD;
    }

    bool readPending(int &rc, wint_t &wch) {
        timeout(0);
        rc = get_wch(&wch);
        timeout(kPollMs);
        return rc != ERR;
    }

    // ----- state helpers ---------------------------------------------------

    View &view() { return views_.back(); }

    void setMessage(const std::string &text, int seconds = 4) {
        message_ = text;
        messageUntil_ = Clock::now() + std::chrono::seconds(seconds);
    }

    int listRows() const {
        int maxY, maxX;
        getmaxyx(stdscr, maxY, maxX);
        (void)maxX;
        return std::max(1, maxY - 3);
    }

    void pollBackground() {
        auto now = Clock::now();

        uint64_t gen = player_.generation();
        if (gen != lastPlayerGen_) {
            lastPlayerGen_ = gen;
            dirty_ = true;
        }
        std::string err = player_.takeError();
        if (!err.empty()) {
            setMessage(err);
            dirty_ = true;
        }

        uint64_t version = index_->version();
        if (version != lastVersion_ &&
            now - lastIndexRefresh_ >= std::chrono::milliseconds(250)) {
            lastVersion_ = version;
            lastIndexRefresh_ = now;
            rebuild(views_.front(), true);
            dirty_ = true;
        }
        int progress = index_->buildProgress();
        bool building = index_->isBuilding();
        if (progress != lastProgress_ || building != lastBuilding_) {
            lastProgress_ = progress;
            lastBuilding_ = building;
            dirty_ = true;
        }

        if (!message_.empty() && now >= messageUntil_) {
            message_.clear();
            dirty_ = true;
        }
    }

    // ----- list contents ---------------------------------------------------

    Item placeItem(const Place &p) const {
        Item item;
        item.kind = ItemKind::Place;
        item.place = p;
        item.count = index_->placeRadioCount(p.id);
        return item;
    }

    static Item radioItem(const RadioEntry &r) {
        Item item;
        item.kind = ItemKind::Radio;
        item.radio = r;
        item.norm = normalizeForSearch(r.radio_title);
        return item;
    }

    void rebuild(View &v, bool keepSelection) {
        std::string selectedKey;
        if (keepSelection && v.sel >= 0 &&
            v.sel < static_cast<int>(v.items.size())) {
            selectedKey = v.items[v.sel].key();
        }

        std::vector<Item> items;
        std::string q = trim(v.query);
        if (v.root) {
            if (q.empty()) {
                for (const Place &p : index_->places()) {
                    items.push_back(placeItem(p));
                }
            } else {
                std::vector<Item> weak;
                std::vector<RadioEntry> radios =
                    index_->searchRadios(q, kMaxRadioResults);
                for (const auto &hit :
                     index_->searchPlaces(q, kMaxPlaceResults)) {
                    if (hit.score >= kFuzzyScore && !radios.empty()) {
                        continue;
                    }
                    (hit.score < kStrongPlaceScore ? items : weak)
                        .push_back(placeItem(hit.place));
                }
                for (const auto &r : radios) {
                    items.push_back(radioItem(r));
                }
                for (auto &w : weak) {
                    items.push_back(std::move(w));
                }
            }
        } else if (q.empty()) {
            items = v.source;
        } else {
            QueryMatcher matcher(q);
            std::vector<std::pair<int, size_t>> hits;
            for (int pass = 0; pass < 2 && hits.empty(); ++pass) {
                if (pass == 1 && !matcher.canFuzzy()) {
                    break;
                }
                for (size_t i = 0; i < v.source.size(); ++i) {
                    int s = pass == 0
                                ? matcher.score(v.source[i].norm, "")
                                : matcher.fuzzyScore(v.source[i].norm, "");
                    if (s >= 0) {
                        hits.push_back({s, i});
                    }
                }
            }
            std::stable_sort(hits.begin(), hits.end(),
                             [](const auto &a, const auto &b) {
                                 return a.first < b.first;
                             });
            for (const auto &h : hits) {
                items.push_back(v.source[h.second]);
            }
        }
        v.items = std::move(items);

        int found = -1;
        if (!selectedKey.empty()) {
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (v.items[i].key() == selectedKey) {
                    found = static_cast<int>(i);
                    break;
                }
            }
        }
        if (found >= 0) {
            v.sel = found;
        } else if (!keepSelection) {
            v.sel = 0;
            v.top = 0;
        }
        clampSelection(v);
    }

    void clampSelection(View &v) {
        int n = static_cast<int>(v.items.size());
        v.sel = n == 0 ? 0 : std::clamp(v.sel, 0, n - 1);
        int rows = listRows();
        int so = std::min(kScrollOff, (rows - 1) / 2);
        if (v.sel < v.top + so) {
            v.top = v.sel - so;
        }
        if (v.sel > v.top + rows - 1 - so) {
            v.top = v.sel - (rows - 1 - so);
        }
        v.top = std::clamp(v.top, 0, std::max(0, n - rows));
    }

    void moveTo(int index) {
        view().sel = index;
        clampSelection(view());
    }

    void moveBy(int delta) { moveTo(view().sel + delta); }

    // ----- actions ---------------------------------------------------------

    void activate() {
        View &v = view();
        if (v.items.empty()) {
            return;
        }
        const Item item = v.items[v.sel];

        if (item.kind == ItemKind::Radio) {
            std::vector<RadioEntry> queue;
            int queueIndex = 0;
            for (int i = 0; i < static_cast<int>(v.items.size()); ++i) {
                if (v.items[i].kind != ItemKind::Radio) {
                    continue;
                }
                if (i == v.sel) {
                    queueIndex = static_cast<int>(queue.size());
                }
                queue.push_back(v.items[i].radio);
            }
            player_.play(item.radio, std::move(queue), queueIndex);
            setMessage("Cargando " + item.radio.radio_title + "…");
            return;
        }

        std::vector<RadioEntry> radios = index_->radiosInPlace(item.place.id);
        if (radios.empty()) {
            // Not indexed yet: fall back to the page itself.
            setMessage("Cargando " + item.place.title + "…");
            render();
            radios = fetchPlaceRadios(item.place.id, item.place.title,
                                      item.place.country);
        }
        if (radios.empty()) {
            setMessage("Sin emisoras en " + item.place.title);
            return;
        }

        View next;
        next.root = false;
        next.place = item.place;
        for (const auto &r : radios) {
            next.source.push_back(radioItem(r));
        }
        message_.clear();
        views_.push_back(std::move(next));
        rebuild(views_.back(), false);
    }

    void goBack() {
        if (views_.size() > 1) {
            views_.pop_back();
        }
    }

    void executeCommand(const std::string &raw) {
        std::string cmd = trim(raw);
        message_.clear();
        if (cmd.empty()) {
            return;
        }
        if (std::all_of(cmd.begin(), cmd.end(),
                        [](char c) { return c >= '0' && c <= '9'; })) {
            moveTo(std::max(1, std::atoi(cmd.c_str())) - 1);
        } else if (cmd == "q" || cmd == "q!" || cmd == "qa" || cmd == "qa!" ||
                   cmd == "quit" || cmd == "x" || cmd == "wq") {
            running_ = false;
        } else if (cmd == "pause" || cmd == "play") {
            player_.togglePause();
        } else if (cmd == "next") {
            player_.next();
        } else if (cmd == "prev" || cmd == "previous") {
            player_.prev();
        } else if (cmd == "stop") {
            player_.stop();
        } else if (cmd == "help" || cmd == "h") {
            showHelp_ = true;
        } else {
            setMessage("E492: comando no reconocido: " + cmd);
        }
    }

    // ----- key handling ----------------------------------------------------

    void handleKey(const Key &key) {
        if (key.isCode(KEY_RESIZE)) {
            clampSelection(view());
            return;
        }
        if (showHelp_) {
            showHelp_ = false;
            return;
        }
        if (key.is(3) && mode_ == Mode::Normal) { // Ctrl-C
            running_ = false;
            return;
        }
        switch (mode_) {
        case Mode::Search:
            searchKey(key);
            break;
        case Mode::Command:
            commandKey(key);
            break;
        case Mode::Normal:
            normalKey(key);
            break;
        }
    }

    void enterSearch() {
        mode_ = Mode::Search;
        searchEdit_.set(view().query);
    }

    void searchKey(const Key &key) {
        View &v = view();
        if (key.esc()) {
            mode_ = Mode::Normal;
        } else if (key.is(3)) { // Ctrl-C: drop the filter and leave
            v.query.clear();
            rebuild(v, false);
            mode_ = Mode::Normal;
        } else if (key.enter()) {
            mode_ = Mode::Normal;
            activate();
        } else if (key.isCode(KEY_DOWN) || key.ctrl('n') || key.ctrl('j')) {
            moveBy(1);
        } else if (key.isCode(KEY_UP) || key.ctrl('p')) {
            moveBy(-1);
        } else if (key.isCode(KEY_NPAGE)) {
            moveBy(listRows());
        } else if (key.isCode(KEY_PPAGE)) {
            moveBy(-listRows());
        } else if (key.backspace() && searchEdit_.text.empty()) {
            mode_ = Mode::Normal;
        } else {
            std::wstring before = searchEdit_.text;
            if (searchEdit_.apply(key) && searchEdit_.text != before) {
                v.query = searchEdit_.utf8();
                rebuild(v, false);
            }
        }
    }

    void commandKey(const Key &key) {
        if (key.esc() || key.is(3)) {
            mode_ = Mode::Normal;
        } else if (key.enter()) {
            mode_ = Mode::Normal;
            executeCommand(commandEdit_.utf8());
        } else if (key.backspace() && commandEdit_.text.empty()) {
            mode_ = Mode::Normal;
        } else {
            commandEdit_.apply(key);
        }
    }

    void normalKey(const Key &key) {
        // Counts: 5j, 12G, ...
        if (!key.code && ((key.c >= '1' && key.c <= '9') ||
                          (key.c == '0' && !count_.empty()))) {
            if (count_.size() < 6) {
                count_ += static_cast<char>(key.c);
            }
            return;
        }
        bool hasCount = !count_.empty();
        int n = hasCount ? std::atoi(count_.c_str()) : 1;
        count_.clear();

        if (pending_ != 0) {
            wchar_t first = pending_;
            pending_ = 0;
            if (first == 'g' && key.is('g')) {
                moveTo(hasCount ? n - 1 : 0);
            } else if (first == 'Z' && (key.is('Z') || key.is('Q'))) {
                running_ = false;
            } else if (first == 'z' && (key.is('z') || key.is('.'))) {
                View &v = view();
                v.top = v.sel - listRows() / 2;
                clampSelection(v);
            }
            return;
        }

        View &v = view();
        int rows = listRows();
        int total = static_cast<int>(v.items.size());
        int visible = std::max(0, std::min(rows, total - v.top));

        if (key.is('j') || key.isCode(KEY_DOWN) || key.ctrl('n')) {
            moveBy(n);
        } else if (key.is('k') || key.isCode(KEY_UP) || key.ctrl('p')) {
            moveBy(-n);
        } else if (key.is('g') || key.is('Z') || key.is('z')) {
            pending_ = static_cast<wchar_t>(key.c);
            if (hasCount) {
                count_ = std::to_string(n);
            }
        } else if (key.is('G')) {
            moveTo(hasCount ? n - 1 : total - 1);
        } else if (key.ctrl('d')) {
            v.top += rows / 2;
            moveBy(rows / 2);
        } else if (key.ctrl('u')) {
            v.top -= rows / 2;
            moveBy(-(rows / 2));
        } else if (key.ctrl('f') || key.isCode(KEY_NPAGE)) {
            v.top += rows;
            moveBy(rows);
        } else if (key.ctrl('b') || key.isCode(KEY_PPAGE)) {
            v.top -= rows;
            moveBy(-rows);
        } else if (key.isCode(KEY_HOME)) {
            moveTo(0);
        } else if (key.isCode(KEY_END)) {
            moveTo(total - 1);
        } else if (key.is('H')) {
            moveTo(v.top);
        } else if (key.is('M')) {
            moveTo(v.top + visible / 2);
        } else if (key.is('L')) {
            moveTo(v.top + visible - 1);
        } else if (key.is('l') || key.is('o') || key.isCode(KEY_RIGHT) ||
                   key.enter()) {
            activate();
        } else if (key.is('h') || key.isCode(KEY_LEFT) || key.backspace()) {
            goBack();
        } else if (key.is('/') || key.is('i')) {
            enterSearch();
        } else if (key.is(':')) {
            mode_ = Mode::Command;
            commandEdit_.clear();
        } else if (key.is('?')) {
            showHelp_ = true;
        } else if (key.esc()) {
            if (!v.query.empty()) {
                v.query.clear();
                rebuild(v, false);
            }
        } else if (key.is(' ') || key.is('p')) {
            player_.togglePause();
        } else if (key.is('n') || key.is(']') || key.is('>') ||
                   key.isCode(KEY_NEXT)) {
            player_.next();
        } else if (key.is('N') || key.is('[') || key.is('<') ||
                   key.isCode(KEY_PREVIOUS)) {
            player_.prev();
        } else if (key.is('S')) {
            player_.stop();
        } else if (key.is('q') || key.is('Q')) {
            running_ = false;
        }
    }

    // ----- drawing ---------------------------------------------------------

    void putText(int y, int x, const std::string &text, int maxCols,
                 attr_t attr = A_NORMAL) {
        if (maxCols <= 0) {
            return;
        }
        attron(attr);
        mvaddstr(y, x, fitWidth(text, maxCols).c_str());
        attroff(attr);
    }

    void fillRow(int y, int x, int cols, attr_t attr) {
        attron(attr);
        mvaddstr(y, x, std::string(cols, ' ').c_str());
        attroff(attr);
    }

    void drawTitleBar(int cols) {
        const View &v = view();
        std::string left = " Radio Garden · " + country_;
        if (!v.root) {
            left += "  ▸  " + v.place.title;
        }

        std::string right;
        if (!v.items.empty()) {
            right = std::to_string(v.sel + 1) + "/" +
                    std::to_string(v.items.size()) + "  ";
        }
        if (index_->isBuilding()) {
            right += "⟳ indexando " + std::to_string(index_->buildProgress()) +
                     "/" + std::to_string(index_->buildTotal()) + "  ";
        } else {
            right += std::to_string(index_->corpusSize()) + " emisoras  ";
        }

        attr_t bar = A_REVERSE | A_BOLD;
        fillRow(0, 0, cols, bar);
        int rightWidth = textWidth(right);
        putText(0, std::max(0, cols - rightWidth), right, rightWidth, bar);
        putText(0, 0, left, cols - rightWidth - 1, bar);
    }

    void drawList(int cols, const PlayerBridge::Snapshot &snap) {
        const View &v = view();
        int rows = listRows();

        if (v.items.empty()) {
            std::string msg;
            if (!trim(v.query).empty()) {
                msg = "Sin resultados para «" + trim(v.query) + "»";
                if (index_->isBuilding()) {
                    msg += " (todavía indexando…)";
                }
            } else {
                msg = "Sin elementos";
            }
            putText(2, 2, msg, cols - 3, style_.dim);
            return;
        }

        for (int row = 0; row < rows; ++row) {
            int idx = v.top + row;
            if (idx >= static_cast<int>(v.items.size())) {
                break;
            }
            const Item &item = v.items[idx];
            int y = 1 + row;
            bool selected = idx == v.sel;
            bool playing = item.kind == ItemKind::Radio && snap.active &&
                           item.radio.id == snap.entry.id;

            std::string right;
            if (item.kind == ItemKind::Radio) {
                if (v.root) {
                    right = item.radio.place_title;
                }
            } else if (item.count > 0) {
                right = std::to_string(item.count) +
                        (item.count == 1 ? " emisora" : " emisoras");
            }
            int rightWidth = right.empty() ? 0 : textWidth(right) + 2;

            attr_t base = A_NORMAL;
            attr_t secondary = style_.dim;
            if (selected) {
                base = playing ? A_REVERSE | A_BOLD : A_REVERSE;
                secondary = A_REVERSE;
                fillRow(y, 0, cols, A_REVERSE);
            } else if (playing) {
                base = style_.playing;
            } else if (item.kind == ItemKind::Place) {
                base = style_.place;
            }

            std::string marker = playing ? (snap.paused ? "‖ " : "▶ ")
                                 : item.kind == ItemKind::Place ? "▸ "
                                                                : "  ";
            std::string title = item.kind == ItemKind::Radio
                                    ? item.radio.radio_title
                                    : item.place.title;
            putText(y, 0, " " + marker, 3, base);
            putText(y, 3, title, cols - 3 - rightWidth, base);
            if (rightWidth > 0) {
                putText(y, cols - rightWidth + 1, right, rightWidth - 2,
                        secondary);
            }
        }
    }

    void drawNowPlaying(int y, int cols, const PlayerBridge::Snapshot &snap) {
        attr_t bar = A_REVERSE;
        fillRow(y, 0, cols, bar);
        std::string text;
        if (!snap.active || snap.entry.id.empty()) {
            text = " ■ Nada en reproducción";
        } else {
            text = snap.paused ? " ‖ " : " ▶ ";
            if (!snap.song.empty()) {
                text += snap.song + "  ·  ";
            }
            text += snap.entry.radio_title + "  —  " + snap.entry.place_title;
        }
        putText(y, 0, text, cols, bar);
    }

    void drawCommandLine(int y, int cols) {
        std::string pendingText = count_;
        if (pending_ != 0) {
            appendUtf8(pendingText, static_cast<uint32_t>(pending_));
        }
        int pendingWidth = textWidth(pendingText);
        if (pendingWidth > 0) {
            putText(y, cols - pendingWidth - 1, pendingText, pendingWidth);
        }

        int avail = cols - pendingWidth - 2;
        if (mode_ == Mode::Search) {
            putText(y, 0, "/" + searchEdit_.utf8(), avail);
        } else if (mode_ == Mode::Command) {
            putText(y, 0, ":" + commandEdit_.utf8(), avail);
        } else if (!message_.empty()) {
            putText(y, 0, message_, avail, A_BOLD);
        } else if (!trim(view().query).empty()) {
            putText(y, 0, "/" + view().query + "   (Esc: limpiar)", avail,
                    style_.dim);
        } else {
            putText(y, 0, "/ buscar   : comando   ? ayuda   q salir", avail,
                    style_.dim);
        }
    }

    void drawHelp(int maxY, int maxX) {
        static const char *const lines[] = {
            " NAVEGAR",
            "   j k ↓ ↑          mover (admite contador: 5j)",
            "   gg G {n}G :{n}   inicio / final / línea n",
            "   Ctrl-d Ctrl-u    media página     Ctrl-f Ctrl-b  página",
            "   H M L   zz       cima / medio / fondo · centrar",
            "   l → Enter        abrir pueblo / reproducir",
            "   h ← Backspace    volver",
            " BUSCAR (en vivo)",
            "   / i              buscar; con varias palabras coinciden todas",
            "   Enter            abrir / reproducir la selección",
            "   Esc              salir de la búsqueda · en Normal: limpiar filtro",
            "   ↑ ↓ Ctrl-n/p     mover la selección mientras escribes",
            "   Ctrl-w Ctrl-u    borrar palabra / hasta el inicio",
            " REPRODUCCIÓN",
            "   Space p          pausa      n ]  siguiente     N [  anterior",
            "   S                parar",
            " COMANDOS",
            "   :q :next :prev :pause :stop :help       q  ZZ  salir",
            " Pulsa cualquier tecla para cerrar",
        };
        const int count = static_cast<int>(sizeof(lines) / sizeof(lines[0]));
        int width = 0;
        for (const char *line : lines) {
            width = std::max(width, textWidth(line));
        }
        width = std::min(width + 2, maxX);
        int height = std::min(count + 2, maxY);
        int top = (maxY - height) / 2;
        int left = (maxX - width) / 2;
        for (int row = 0; row < height; ++row) {
            fillRow(top + row, left, width, A_REVERSE);
            int li = row - 1;
            if (li >= 0 && li < count) {
                putText(top + row, left, lines[li], width, A_REVERSE);
            }
        }
    }

    void render() {
        int maxY, maxX;
        getmaxyx(stdscr, maxY, maxX);
        erase();
        if (maxY < 5 || maxX < 20) {
            mvaddstr(0, 0, "Terminal demasiado pequeño");
            refresh();
            return;
        }

        clampSelection(view());
        PlayerBridge::Snapshot snap = player_.snapshot();
        drawTitleBar(maxX);
        drawList(maxX, snap);
        drawNowPlaying(maxY - 2, maxX, snap);
        drawCommandLine(maxY - 1, maxX);
        if (showHelp_) {
            drawHelp(maxY, maxX);
        }

        if (mode_ != Mode::Normal && !showHelp_) {
            const LineEdit &edit =
                mode_ == Mode::Search ? searchEdit_ : commandEdit_;
            int col = 1 + textWidth(edit.utf8Prefix(edit.cur));
            move(maxY - 1, std::min(col, maxX - 1));
            curs_set(1);
        } else {
            curs_set(0);
        }
        refresh();
    }

    std::string country_;
    std::unique_ptr<RadioIndex> index_;
    PlayerBridge player_;
    Style style_;

    std::vector<View> views_;
    Mode mode_ = Mode::Normal;
    LineEdit searchEdit_;
    LineEdit commandEdit_;
    std::string count_;
    wchar_t pending_ = 0;
    std::string message_;
    Clock::time_point messageUntil_;
    bool showHelp_ = false;
    bool running_ = true;
    bool dirty_ = true;

    uint64_t lastVersion_ = 0;
    uint64_t lastPlayerGen_ = 0;
    int lastProgress_ = 0;
    bool lastBuilding_ = false;
    Clock::time_point lastIndexRefresh_;
};

struct Curses {
    Curses() {
        setlocale(LC_ALL, "");
        initscr();
        raw();
        noecho();
        keypad(stdscr, TRUE);
        set_escdelay(25);
        timeout(kPollMs);
        curs_set(0);
    }
    ~Curses() { endwin(); }
};

void onTerminate(int) { g_mpris_quit = true; }

} // namespace

void runTUI(const std::string &defaultCountry) {
    // Background HTTP errors are written to stderr, which would scribble over
    // the screen; keep them in a log instead.
    std::string logPath = getCacheDir() + "/tui.log";
    if (!freopen(logPath.c_str(), "a", stderr)) {
        (void)freopen("/dev/null", "w", stderr);
    }
    std::signal(SIGTERM, onTerminate);
    std::signal(SIGHUP, onTerminate);

    Curses curses;
    mvaddstr(0, 0, "Radio Garden · cargando lugares…");
    refresh();

    App app(defaultCountry, std::make_unique<RadioIndex>(defaultCountry));
    app.run();
}
