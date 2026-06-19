#include "state.hpp"

#include "paths.hpp"

#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>

std::optional<PlaybackState> loadState() {
    std::ifstream f(getStatePath());
    if (!f.good()) {
        return std::nullopt;
    }

    try {
        nlohmann::json j;
        f >> j;
        PlaybackState state;
        state.country = j.value("country", "");
        state.place_id = j.value("place_id", "");
        state.place_title = j.value("place_title", "");
        state.radio_href = j.value("radio_href", "");
        state.radio_title = j.value("radio_title", "");
        state.radio_id = j.value("radio_id", "");
        if (state.radio_id.empty() && !state.radio_href.empty()) {
            state.radio_id = Radio::extractId(state.radio_href);
        }
        if (state.place_id.empty() || state.radio_id.empty()) {
            return std::nullopt;
        }
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

void saveState(const PlaybackState &state) {
    nlohmann::json j;
    j["country"] = state.country;
    j["place_id"] = state.place_id;
    j["place_title"] = state.place_title;
    j["radio_href"] = state.radio_href;
    j["radio_title"] = state.radio_title;
    j["radio_id"] = state.radio_id;
    std::time_t now = std::time(nullptr);
    j["updated_at"] = now;

    std::ofstream f(getStatePath());
    f << j.dump(2);
}

bool validateState(const PlaybackState &state) {
    std::ifstream f(getPlacesPath());
    if (!f.good()) {
        return false;
    }
    try {
        nlohmann::json data = nlohmann::json::parse(f);
        for (const auto &item : data["data"]["list"]) {
            if (item["id"].get<std::string>() == state.place_id) {
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}
