#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "radio.hpp"
#include "state.hpp"

struct StreamMetadata {
    std::string title;
    std::string artist;
    bool valid = false;
};

class PlaybackQueue {
  public:
    void set(const std::vector<RadioEntry> &entries, int startIndex);
    bool next(RadioEntry &out);
    bool prev(RadioEntry &out);
    RadioEntry current() const;
    int currentIndex() const;
    const std::vector<RadioEntry> &all() const;

  private:
    std::vector<RadioEntry> entries_;
    int index_{0};
};

class MpvClient {
  public:
    explicit MpvClient(const std::string &socketPath);

    bool isRunning() const;
    bool launch(const std::string &url);
    bool loadFile(const std::string &url);
    bool cyclePause();
    bool getPause(bool &paused) const;
    bool quit();
    void stop();
    StreamMetadata getStreamMetadata() const;

  private:
    bool sendCommand(const std::string &command) const;
    bool sendCommandWithReply(const std::string &command,
                              std::string &reply) const;
    bool waitForSocket(int retriesMs) const;

    std::string socket_path_;
    pid_t pid_{-1};
};

class PlayerController {
  public:
    static PlayerController &instance();

    void play(const RadioEntry &entry, const std::vector<RadioEntry> &queue,
              int queueIndex);
    bool playOk(const RadioEntry &entry, const std::vector<RadioEntry> &queue,
                int queueIndex);
    void resume(const PlaybackState &state);
    void togglePause();
    void next();
    void prev();
    void stop();

    bool isPaused() const;
    bool hasActivePlayback() const;
    RadioEntry currentEntry() const;
    PlaybackState currentState() const;
    std::string currentSong() const;
    bool pollMetadata();

    std::mutex &mutex() { return mutex_; }

  private:
    PlayerController();

    mutable std::mutex mutex_;
    MpvClient mpv_;
    PlaybackQueue queue_;
    RadioEntry current_;
    bool active_{false};
    std::string currentSong_;
    std::string lastSongId_;
};
