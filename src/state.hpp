#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "radio.hpp"

struct PlaybackState {
    std::string country;
    std::string place_id;
    std::string place_title;
    std::string radio_href;
    std::string radio_title;
    std::string radio_id;
};

std::optional<PlaybackState> loadState();
void saveState(const PlaybackState &state);
bool validateState(const PlaybackState &state);
