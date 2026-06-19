#include "mpris.hpp"

#include "player.hpp"

#include <pthread.h>
#include <string>
#include <systemd/sd-bus.h>
#include <thread>

std::atomic<bool> g_mpris_quit{false};

static sd_bus *g_bus = nullptr;
static std::thread g_mpris_thread;
static std::atomic<bool> g_mpris_running{false};

static std::string g_title;
static std::string g_artist;
static std::string g_status = "Stopped";
static pthread_mutex_t g_mpris_mutex = PTHREAD_MUTEX_INITIALIZER;

static int handlePlay(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)m;
    (void)userdata;
    (void)ret_error;
    PlayerController &pc = PlayerController::instance();
    if (pc.isPaused()) {
        pc.togglePause();
    }
    pthread_mutex_lock(&g_mpris_mutex);
    g_status = "Playing";
    pthread_mutex_unlock(&g_mpris_mutex);
    return sd_bus_reply_method_return(m, nullptr);
}

static int handlePause(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;
    PlayerController &pc = PlayerController::instance();
    if (!pc.isPaused()) {
        pc.togglePause();
    }
    pthread_mutex_lock(&g_mpris_mutex);
    g_status = "Paused";
    pthread_mutex_unlock(&g_mpris_mutex);
    return sd_bus_reply_method_return(m, nullptr);
}

static int handlePlayPause(sd_bus_message *m, void *userdata,
                           sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;
    PlayerController &pc = PlayerController::instance();
    pc.togglePause();
    pthread_mutex_lock(&g_mpris_mutex);
    g_status = pc.isPaused() ? "Paused" : "Playing";
    pthread_mutex_unlock(&g_mpris_mutex);
    return sd_bus_reply_method_return(m, nullptr);
}

static int handleNext(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)m;
    (void)userdata;
    (void)ret_error;
    PlayerController::instance().next();
    auto entry = PlayerController::instance().currentEntry();
    notifyMprisMetadata(entry.radio_title, entry.place_title, true);
    return sd_bus_reply_method_return(m, nullptr);
}

static int handlePrev(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)m;
    (void)userdata;
    (void)ret_error;
    PlayerController::instance().prev();
    auto entry = PlayerController::instance().currentEntry();
    notifyMprisMetadata(entry.radio_title, entry.place_title, true);
    return sd_bus_reply_method_return(m, nullptr);
}

static int handleQuit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;
    g_mpris_quit = true;
    PlayerController::instance().stop();
    return sd_bus_reply_method_return(m, nullptr);
}

static int handleStop(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;
    PlayerController::instance().stop();
    pthread_mutex_lock(&g_mpris_mutex);
    g_status = "Stopped";
    pthread_mutex_unlock(&g_mpris_mutex);
    return sd_bus_reply_method_return(m, nullptr);
}

static int getProperty(sd_bus *bus, const char *path, const char *interface,
                       const char *property, sd_bus_message *reply,
                       void *userdata, sd_bus_error *ret_error) {
    (void)bus;
    (void)path;
    (void)userdata;
    (void)ret_error;

    pthread_mutex_lock(&g_mpris_mutex);

    if (strcmp(interface, "org.mpris.MediaPlayer2") == 0) {
        if (strcmp(property, "Identity") == 0) {
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 's', "Radio Garden CLI");
        }
        if (strcmp(property, "CanQuit") == 0) {
            int v = 1;
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 'b', &v);
        }
        if (strcmp(property, "CanRaise") == 0) {
            int v = 0;
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 'b', &v);
        }
        if (strcmp(property, "DesktopEntry") == 0) {
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 's', "radio-garden-cli");
        }
    }

    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (strcmp(property, "PlaybackStatus") == 0) {
            const char *status = g_status.c_str();
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 's', status);
        }
        if (strcmp(property, "Rate") == 0) {
            double rate = 1.0;
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 'd', &rate);
        }
        if (strcmp(property, "Metadata") == 0) {
            sd_bus_message_open_container(reply, 'a', "{sv}");
            sd_bus_message_open_container(reply, 'e', "sv");
            const char *key = "xesam:title";
            sd_bus_message_append_basic(reply, 's', key);
            const char *title = g_title.c_str();
            sd_bus_message_open_container(reply, 'v', "s");
            sd_bus_message_append_basic(reply, 's', title);
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);

            sd_bus_message_open_container(reply, 'e', "sv");
            key = "xesam:artist";
            sd_bus_message_append_basic(reply, 's', key);
            const char *artist = g_artist.c_str();
            sd_bus_message_open_container(reply, 'v', "s");
            sd_bus_message_append_basic(reply, 's', artist);
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);
            pthread_mutex_unlock(&g_mpris_mutex);
            return 0;
        }
        if (strcmp(property, "CanControl") == 0 ||
            strcmp(property, "CanPlay") == 0 ||
            strcmp(property, "CanPause") == 0 ||
            strcmp(property, "CanGoNext") == 0 ||
            strcmp(property, "CanGoPrevious") == 0) {
            int v = 1;
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 'b', &v);
        }
        if (strcmp(property, "Volume") == 0) {
            double vol = 1.0;
            pthread_mutex_unlock(&g_mpris_mutex);
            return sd_bus_message_append_basic(reply, 'd', &vol);
        }
    }

    pthread_mutex_unlock(&g_mpris_mutex);
    return sd_bus_error_setf(ret_error, SD_BUS_ERROR_UNKNOWN_PROPERTY,
                             "Unknown property %s", property);
}

static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Play", "", "", handlePlay, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", handlePause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", handlePlayPause,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Next", "", "", handleNext, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", handlePrev, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", handleStop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", handleQuit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PlaybackStatus", "s", getProperty, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Rate", "d", getProperty, 0, 0),
    SD_BUS_PROPERTY("Metadata", "a{sv}", getProperty, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Volume", "d", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanControl", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanPlay", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanPause", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanGoNext", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanGoPrevious", "b", getProperty, 0, 0),
    SD_BUS_VTABLE_END};

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Identity", "s", getProperty, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanQuit", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("CanRaise", "b", getProperty, 0, 0),
    SD_BUS_PROPERTY("DesktopEntry", "s", getProperty, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

static void mprisLoop() {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_open_user(&g_bus);
    if (r < 0) {
        return;
    }

    r = sd_bus_request_name(g_bus, "org.mpris.MediaPlayer2.radio_garden_cli",
                            SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) {
        sd_bus_unref(g_bus);
        g_bus = nullptr;
        return;
    }

    r = sd_bus_add_object_vtable(g_bus, nullptr, "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2", root_vtable,
                                 nullptr);
    if (r < 0) {
        sd_bus_unref(g_bus);
        g_bus = nullptr;
        return;
    }

    r = sd_bus_add_object_vtable(g_bus, nullptr, "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2.Player", player_vtable,
                                 nullptr);
    if (r < 0) {
        sd_bus_unref(g_bus);
        g_bus = nullptr;
        return;
    }

    g_mpris_running = true;
    while (g_mpris_running) {
        r = sd_bus_process(g_bus, nullptr);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            sd_bus_wait(g_bus, 100000);
        }
    }

    sd_bus_error_free(&error);
    if (g_bus) {
        sd_bus_unref(g_bus);
        g_bus = nullptr;
    }
}

void startMprisServer() {
    if (g_mpris_thread.joinable()) {
        return;
    }
    g_mpris_thread = std::thread(mprisLoop);
}

void stopMprisServer() {
    g_mpris_running = false;
    if (g_mpris_thread.joinable()) {
        g_mpris_thread.join();
    }
}

void notifyMprisMetadata(const std::string &title, const std::string &artist,
                         bool playing) {
    pthread_mutex_lock(&g_mpris_mutex);
    g_title = title;
    g_artist = artist;
    g_status = playing ? "Playing" : "Paused";
    pthread_mutex_unlock(&g_mpris_mutex);

    if (!g_bus) {
        return;
    }
    sd_bus_emit_properties_changed(
        g_bus, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
        "PlaybackStatus", "Metadata", nullptr);
}
