#include "radio.cpp"
#include "requests.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>

std::vector<Stations> readStations() {
    std::ifstream f("places.json");
    nlohmann::json jsonData;
    if (!f.good()) {
        // Download the file if missing
        std::string buffer;
        std::string url = "https://radio.garden/api/ara/content/places";
        get(url, buffer, true, "places.json");
        jsonData = nlohmann::json::parse(buffer);
    } else {
        jsonData = nlohmann::json::parse(f);
    }

    auto list = jsonData["data"]["list"];

    std::vector<Stations> stationsList;
    for (auto item : list) {
        std::string title = item["title"];
        // std::string url = item["url"];
        std::string country = item["country"];
        std::string id = item["id"];
        // double latitude = item["geo"][1];
        // double longitude = item["geo"][0];

        Stations stations(id, title, country);
        stationsList.push_back(stations);
    }

    return stationsList;
}

void printStations() {
    std::vector<Stations> stationsList = readStations();
    for (Stations stations : stationsList) {
        std::cout << stations.title << " -- " << stations.country << std::endl;
    }

    std::cout << "Total stations: " << stationsList.size() << std::endl;
}

Stations filterByPlace(std::string place) {
    std::vector<Stations> stationsList = readStations();
    std::vector<Stations> filteredStations;

    for (Stations stations : stationsList) {
        // TODO: Find by regex
        if (stations.title == place) {
            filteredStations.push_back(stations);
        }
    }

    int idx = 0;
    std::cout << "Found stations at " << place << ": " << std::endl;
    if (filteredStations.size() == 0) {
        std::cout << "No stations found" << std::endl;
        exit(0);
    } else if (filteredStations.size() == 1) {
        std::cout << "1 station found" << std::endl;
    } else {
        std::cout << filteredStations.size() << " stations found" << std::endl;
        int i = 0;
        for (Stations stations : filteredStations) {
            std::cout << "[" << i << "] " << stations.title << " -- "
                      << stations.country << std::endl;
            i++;
        }
        std::cout << "Select a station: ";
        std::cin >> idx;

        if (idx > i) {
            std::cout << "Invalid index" << std::endl;
            exit(0);
        }
    }

    return filteredStations.at(idx);
}

void helpMenu() {
    std::cout << "Usage: radio [LOCATION]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help\t\tShow this help menu" << std::endl;
    std::cout << "  -l, --list\t\tList all radios" << std::endl;
    std::cout << "  -p, --play <place>\t\tPlay radio from <place>" << std::endl;
    std::cout << "  -v, --version\t\tShow version" << std::endl;
}

void showVersion() { std::cout << "Radio Garden CLI v0.1.0" << std::endl; }

int main(int argc, char **argv) {
    if (argc == 1) {
        helpMenu();
        return 0;
    }

    std::string option = argv[1];

    if (option == "-h" || option == "--help") {
        helpMenu();
    } else if (option == "-v" || option == "--version") {
        showVersion();
    } else if (option == "-l" || option == "--list") {
        printStations();
    } else if (option == "-p" || option == "--play") {
        std::string by = argv[2];
        // searchRadios(by);
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
