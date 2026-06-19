#include "paths.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static std::string homeDir() {
    const char *home = std::getenv("HOME");
    return home ? home : "/tmp";
}

static std::string xdgDir(const char *envVar, const std::string &fallback) {
    const char *val = std::getenv(envVar);
    if (val && val[0] != '\0') {
        return std::string(val);
    }
    return homeDir() + "/" + fallback;
}

std::string getConfigDir() {
    std::string dir = xdgDir("XDG_CONFIG_HOME", ".config") + "/radio-garden-cli";
    ensureDir(dir);
    return dir;
}

std::string getCacheDir() {
    std::string dir = xdgDir("XDG_CACHE_HOME", ".cache") + "/radio-garden-cli";
    ensureDir(dir);
    return dir;
}

std::string getRuntimeDir() {
    std::string dir = xdgDir("XDG_RUNTIME_DIR", ".cache") + "/radio-garden-cli";
    ensureDir(dir);
    return dir;
}

std::string getStatePath() { return getConfigDir() + "/state.json"; }

std::string getPlacesPath() { return getCacheDir() + "/places.json"; }

std::string getMpvSocketPath() { return getRuntimeDir() + "/mpv.sock"; }

std::string getIndexPath(const std::string &country) {
    std::string safe = country;
    for (size_t i = 0; i < safe.size(); ++i) {
        if (safe[i] == ' ') {
            safe[i] = '_';
        }
    }
    return getCacheDir() + "/index_" + safe + ".json";
}

std::string getPlaceCachePath(const std::string &placeId) {
    std::string dir = getCacheDir() + "/places";
    ensureDir(dir);
    return dir + "/" + placeId + ".json";
}

void ensureDir(const std::string &path) {
    mkdir(path.c_str(), 0755);
}
