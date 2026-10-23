#include "player.hpp"

#include "index.hpp"
#include "paths.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

void PlaybackQueue::set(const std::vector<RadioEntry> &entries,
                        int startIndex) {
    entries_ = entries;
    if (entries_.empty()) {
        index_ = 0;
        return;
    }
    if (startIndex < 0) {
        startIndex = 0;
    }
    if (startIndex >= static_cast<int>(entries_.size())) {
        startIndex = static_cast<int>(entries_.size()) - 1;
    }
    index_ = startIndex;
}

bool PlaybackQueue::next(RadioEntry &out) {
    if (entries_.empty()) {
        return false;
    }
    index_ = (index_ + 1) % static_cast<int>(entries_.size());
    out = entries_[index_];
    return true;
}

bool PlaybackQueue::prev(RadioEntry &out) {
    if (entries_.empty()) {
        return false;
    }
    index_ = (index_ - 1 + static_cast<int>(entries_.size())) %
             static_cast<int>(entries_.size());
    out = entries_[index_];
    return true;
}

RadioEntry PlaybackQueue::current() const {
    if (entries_.empty()) {
        return RadioEntry();
    }
    return entries_[index_];
}

int PlaybackQueue::currentIndex() const { return index_; }

const std::vector<RadioEntry> &PlaybackQueue::all() const { return entries_; }

MpvClient::MpvClient(const std::string &socketPath) : socket_path_(socketPath) {}

bool MpvClient::isRunning() const {
    if (pid_ > 0) {
        int status;
        pid_t ret = waitpid(pid_, &status, WNOHANG);
        if (ret == pid_) {
            return false;
        }
        if (ret == 0) {
            return true;
        }
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    bool ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

bool MpvClient::waitForSocket(int retriesMs) const {
    for (int i = 0; i < retriesMs; ++i) {
        if (isRunning()) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

bool MpvClient::sendCommand(const std::string &command) const {
    std::string reply;
    if (!sendCommandWithReply(command, reply)) {
        return false;
    }
    // mpv answers {"error":"success"} or e.g. {"error":"invalid parameter"}.
    return reply.find("\"error\":\"success\"") != std::string::npos;
}

bool MpvClient::sendCommandWithReply(const std::string &command,
                                     std::string &reply) const {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    std::string payload = command;
    if (payload.empty() || payload.back() != '\n') {
        payload += '\n';
    }

    ssize_t written = write(fd, payload.c_str(), payload.size());
    if (written < 0) {
        close(fd);
        return false;
    }

    char buffer[4096];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        reply.assign(buffer, n);
    }
    close(fd);
    return n > 0;
}

static std::string jsonEscape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

bool MpvClient::launch(const std::string &url) {
    stop();
    unlink(socket_path_.c_str());

    pid_ = fork();
    if (pid_ == 0) {
        // mpv must not share the TUI's terminal: it would read the same
        // keystrokes (Enter skips the playlist, [ ] change speed, q quits).
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execlp("mpv", "mpv", "--no-video", "--really-quiet", "--no-terminal",
               "--no-input-default-bindings",
               "--http-header-fields=Referer: https://radio.garden/",
               ("--input-ipc-server=" + socket_path_).c_str(), url.c_str(),
               nullptr);
        _exit(1);
    }
    if (pid_ < 0) {
        return false;
    }
    return waitForSocket(300);
}

bool MpvClient::loadFile(const std::string &url) {
    // The Referer header was set at launch (--http-header-fields) and is kept.
    // Don't pass an options object: since mpv 0.38 the 4th argument is the
    // playlist index, so it fails with "invalid parameter".
    std::string cmd = "{\"command\":[\"loadfile\",\"" + jsonEscape(url) +
                      "\",\"replace\"]}";
    return sendCommand(cmd);
}

bool MpvClient::cyclePause() {
    return sendCommand("{\"command\":[\"cycle\",\"pause\"]}");
}

bool MpvClient::getPause(bool &paused) const {
    std::string reply;
    if (!sendCommandWithReply("{\"command\":[\"get_property\",\"pause\"]}", reply)) {
        return false;
    }
    paused = reply.find("true") != std::string::npos;
    return true;
}

StreamMetadata MpvClient::getStreamMetadata() const {
    StreamMetadata meta;
    std::string reply;
    if (!sendCommandWithReply(
            "{\"command\":[\"get_property\",\"filtered-metadata\"]}", reply)) {
        return meta;
    }
    try {
        auto json = nlohmann::json::parse(reply);
        if (!json.contains("data") || json["data"].is_null()) {
            return meta;
        }
        auto &data = json["data"];
        if (data.contains("icy-title") && !data["icy-title"].is_null()) {
            std::string icy = data["icy-title"].get<std::string>();
            size_t sep = icy.find(" - ");
            if (sep != std::string::npos && sep > 0 &&
                sep + 3 < icy.size()) {
                meta.artist = icy.substr(0, sep);
                meta.title = icy.substr(sep + 3);
            } else {
                meta.title = icy;
            }
            meta.valid = !meta.title.empty();
            return meta;
        }
        if (data.contains("title") && !data["title"].is_null()) {
            meta.title = data["title"].get<std::string>();
            meta.valid = true;
        }
        if (data.contains("artist") && !data["artist"].is_null()) {
            meta.artist = data["artist"].get<std::string>();
        }
    } catch (...) {
    }
    return meta;
}

bool MpvClient::quit() {
    sendCommand("{\"command\":[\"quit\"]}");
    if (pid_ > 0) {
        int status;
        for (int i = 0; i < 20; ++i) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return true;
            }
            usleep(50000);
        }
        kill(pid_, SIGTERM);
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    return true;
}

void MpvClient::stop() { quit(); }

PlayerController &PlayerController::instance() {
    static PlayerController controller;
    return controller;
}

PlayerController::PlayerController() : mpv_(getMpvSocketPath()) {}

void PlayerController::play(const RadioEntry &entry,
                          const std::vector<RadioEntry> &queue, int queueIndex) {
    (void)playOk(entry, queue, queueIndex);
}

bool PlayerController::playOk(const RadioEntry &entry,
                              const std::vector<RadioEntry> &queue,
                              int queueIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.set(queue, queueIndex);
    current_ = entry;
    active_ = true;
    lastSongId_.clear();
    currentSong_.clear();

    std::string url = radioBuildUrl(entry.id);
    bool ok = false;
    if (mpv_.isRunning()) {
        ok = mpv_.loadFile(url);
    } else {
        ok = mpv_.launch(url);
    }
    if (!ok) {
        active_ = false;
        return false;
    }

    PlaybackState state;
    state.country = entry.country;
    state.place_id = entry.place_id;
    state.place_title = entry.place_title;
    state.radio_href = entry.href;
    state.radio_title = entry.radio_title;
    state.radio_id = entry.id;
    saveState(state);
    return true;
}

void PlayerController::resume(const PlaybackState &state) {
    auto radios = fetchPlaceRadios(state.place_id, state.place_title,
                                   state.country);
    if (radios.empty()) {
        return;
    }

    int index = 0;
    for (size_t i = 0; i < radios.size(); ++i) {
        if (radios[i].id == state.radio_id) {
            index = static_cast<int>(i);
            break;
        }
    }

    RadioEntry entry = radios[index];
    play(entry, radios, index);
}

void PlayerController::togglePause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;
    }
    mpv_.cyclePause();
}

void PlayerController::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;
    }
    RadioEntry entry;
    if (!queue_.next(entry)) {
        return;
    }
    current_ = entry;
    lastSongId_.clear();
    currentSong_.clear();
    mpv_.loadFile(radioBuildUrl(entry.id));

    PlaybackState state;
    state.country = entry.country;
    state.place_id = entry.place_id;
    state.place_title = entry.place_title;
    state.radio_href = entry.href;
    state.radio_title = entry.radio_title;
    state.radio_id = entry.id;
    saveState(state);
}

void PlayerController::prev() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;
    }
    RadioEntry entry;
    if (!queue_.prev(entry)) {
        return;
    }
    current_ = entry;
    lastSongId_.clear();
    currentSong_.clear();
    mpv_.loadFile(radioBuildUrl(entry.id));

    PlaybackState state;
    state.country = entry.country;
    state.place_id = entry.place_id;
    state.place_title = entry.place_title;
    state.radio_href = entry.href;
    state.radio_title = entry.radio_title;
    state.radio_id = entry.id;
    saveState(state);
}

void PlayerController::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    mpv_.stop();
    active_ = false;
    lastSongId_.clear();
    currentSong_.clear();
}

bool PlayerController::pollMetadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return false;
    }
    StreamMetadata meta = mpv_.getStreamMetadata();
    if (!meta.valid || meta.title.empty()) {
        return false;
    }
    std::string songId = meta.title + "||" + meta.artist;
    if (songId == lastSongId_) {
        return false;
    }
    lastSongId_ = songId;
    currentSong_ = meta.title;
    return true;
}

std::string PlayerController::currentSong() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSong_;
}

bool PlayerController::isPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return false;
    }
    bool paused = false;
    mpv_.getPause(paused);
    return paused;
}

bool PlayerController::hasActivePlayback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

RadioEntry PlayerController::currentEntry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

PlaybackState PlayerController::currentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaybackState state;
    state.country = current_.country;
    state.place_id = current_.place_id;
    state.place_title = current_.place_title;
    state.radio_href = current_.href;
    state.radio_title = current_.radio_title;
    state.radio_id = current_.id;
    return state;
}
