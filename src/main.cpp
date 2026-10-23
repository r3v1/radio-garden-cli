#include "index.hpp"
#include "mpris.hpp"
#include "paths.hpp"
#include "player.hpp"
#include "radio.hpp"
#include "state.hpp"
#include "tui.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

std::vector<Stations> readStations() {
    auto json = loadPlacesJson();
    std::vector<Stations> stationsList;
    for (const auto &item : json["data"]["list"]) {
        std::string title = item["title"];
        std::string country = item["country"];
        std::string id = item["id"];
        stationsList.emplace_back(id, title, country);
    }
    return stationsList;
}

void printStations() {
    std::vector<Stations> stationsList = readStations();
    for (const Stations &stations : stationsList) {
        std::cout << stations.title << " -- " << stations.country << std::endl;
    }
    std::cout << "Total stations: " << stationsList.size() << std::endl;
}

Stations filterByPlace(const std::string &place) {
    std::vector<Stations> stationsList = readStations();
    std::vector<Stations> filteredStations;

    for (const Stations &stations : stationsList) {
        if (stations.title == place) {
            filteredStations.push_back(stations);
        }
    }

    int idx = 0;
    std::cout << "Found stations at " << place << ": " << std::endl;
    if (filteredStations.empty()) {
        std::cout << "No stations found" << std::endl;
        exit(0);
    } else if (filteredStations.size() == 1) {
        std::cout << "1 station found" << std::endl;
    } else {
        std::cout << filteredStations.size() << " stations found" << std::endl;
        int i = 0;
        for (const Stations &stations : filteredStations) {
            std::cout << "[" << i << "] " << stations.title << " -- "
                      << stations.country << std::endl;
            i++;
        }
        std::cout << "Select a station: ";
        std::cin >> idx;
        if (idx >= i) {
            std::cout << "Invalid index" << std::endl;
            exit(0);
        }
    }

    return filteredStations.at(idx);
}

// Finds radios by name across the whole country (accent/case insensitive).
// A single match plays right away; several are listed to choose from.
int searchAndPlay(const std::string &query) {
    RadioIndex index("Spain");
    index.startBackgroundBuild();
    auto started = std::chrono::steady_clock::now();
    bool reported = false;
    while (index.isBuilding()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!reported && std::chrono::steady_clock::now() - started >
                             std::chrono::seconds(1)) {
            std::cerr << "Indexing radios (first run downloads them)..."
                      << std::endl;
            reported = true;
        }
    }

    std::vector<RadioEntry> results = index.searchRadios(query, 20);
    if (results.empty()) {
        std::cout << "No stations found for \"" << query << "\"" << std::endl;
        return 1;
    }

    size_t choice = 0;
    if (results.size() > 1) {
        for (size_t i = 0; i < results.size(); ++i) {
            std::cout << "[" << i << "] " << results[i].radio_title << " -- "
                      << results[i].place_title << std::endl;
        }
        if (!isatty(STDIN_FILENO)) {
            return 0;
        }
        std::cout << "Select a station: ";
        size_t input = 0;
        if (!(std::cin >> input) || input >= results.size()) {
            std::cout << "Invalid index" << std::endl;
            return 1;
        }
        choice = input;
    }

    const RadioEntry &radio = results[choice];
    std::cout << "Playing " << radio.radio_title << " -- " << radio.place_title
              << std::endl;
    std::string cmd =
        "mpv --no-video --http-header-fields='Referer: https://radio.garden/' " +
        radioBuildUrl(radio.id);
    return system(cmd.c_str()) == 0 ? 0 : 1;
}

void helpMenu() {
    std::cout << "Usage: radio [OPTION]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  (no args)\t\tLaunch TUI (Spain) and resume last station"
              << std::endl;
    std::cout << "  -h, --help\t\tShow this help menu" << std::endl;
    std::cout << "  -l, --list\t\tList all places" << std::endl;
    std::cout << "  -p, --play <place>\tPlay radio from <place>" << std::endl;
    std::cout << "  -s, --search <name>\tFind a station by name and play it"
              << std::endl;
    std::cout << "  -t, --tui\t\tLaunch TUI (Spain)" << std::endl;
    std::cout << "  -v, --version\t\tShow version" << std::endl;
    std::cout << std::endl;
    std::cout << "TUI keys (vim-like, press ? inside for the full list):"
              << std::endl;
    std::cout << "  Move: j/k, gg/G, Ctrl-d/u, 5j; l/Enter open/play, h back"
              << std::endl;
    std::cout << "  Search: / (live), Esc leaves, Enter plays, : commands"
              << std::endl;
    std::cout << "  Media: Space pause, ]/n next, [/N prev, S stop, q quit"
              << std::endl;
}

void showVersion() { std::cout << "Radio Garden CLI v0.3.0" << std::endl; }

int main(int argc, char **argv) {
    ensureDir(getCacheDir());
    ensureDir(getConfigDir());

    if (argc == 1) {
        loadPlacesJson();
        startMprisServer();
        auto state = loadState();
        if (state && validateState(*state)) {
            PlayerController::instance().resume(*state);
            notifyMprisMetadata(state->radio_title, state->place_title, true);
        }
        runTUI("Spain");
        stopMprisServer();
        return 0;
    }

    std::string option = argv[1];

    if (option == "-h" || option == "--help") {
        helpMenu();
    } else if (option == "-v" || option == "--version") {
        showVersion();
    } else if (option == "-l" || option == "--list") {
        printStations();
    } else if (option == "-t" || option == "--tui") {
        loadPlacesJson();
        startMprisServer();
        auto state = loadState();
        if (state && validateState(*state)) {
            PlayerController::instance().resume(*state);
            notifyMprisMetadata(state->radio_title, state->place_title, true);
        }
        runTUI("Spain");
        stopMprisServer();
    } else if (option == "-s" || option == "--search") {
        if (argc < 3) {
            std::cout << "Error: -s/--search requires a name argument"
                      << std::endl;
            helpMenu();
            return 1;
        }
        std::string query = argv[2];
        for (int i = 3; i < argc; ++i) {
            query += " ";
            query += argv[i];
        }
        return searchAndPlay(query);
    } else if (option == "-p" || option == "--play") {
        if (argc < 3) {
            std::cout << "Error: -p/--play requires a place argument"
                      << std::endl;
            helpMenu();
            return 1;
        }
        std::string by = argv[2];
        Stations stations = filterByPlace(by);
        stations.buildStations();
        stations.selectRadio();
        stations.playRadio();
    } else {
        std::cout << "Invalid option" << std::endl;
        helpMenu();
    }
    return 0;
}
