#include "radio.hpp"

#include <ctime>
#include <iostream>

Radio::Radio(const std::string &title, const std::string &href) {
    this->href = href;
    this->title = title;
    this->id = extractId(href);
}

void Radio::print() const { std::cout << this->title << std::endl; }

std::string Radio::extractId(const std::string &href) {
    return href.substr(href.find_last_of('/') + 1);
}

std::string radioBuildUrl(const std::string &radioId) {
    std::time_t now = std::time(nullptr);
    return "https://radio.garden/api/ara/content/listen/" + radioId +
           "/channel.mp3?" + std::to_string(now);
}

std::string Radio::buildUrl() const { return radioBuildUrl(this->id); }

std::vector<RadioEntry>
parseRadiosFromPageJson(const nlohmann::json &json, const std::string &placeId,
                        const std::string &placeTitle,
                        const std::string &country) {
    std::vector<RadioEntry> entries;
    if (!json.contains("data") || !json["data"].contains("content")) {
        return entries;
    }

    for (const auto &content : json["data"]["content"]) {
        if (!content.contains("items")) {
            continue;
        }
        for (const auto &element : content["items"]) {
            if (!element.contains("page")) {
                continue;
            }
            const auto &page = element["page"];
            if (!page.contains("url") || !page.contains("title")) {
                continue;
            }
            RadioEntry entry;
            entry.href = page["url"].get<std::string>();
            entry.radio_title = page["title"].get<std::string>();
            entry.id = Radio::extractId(entry.href);
            entry.place_id = placeId;
            entry.place_title = placeTitle;
            entry.country = country;
            entries.push_back(entry);
        }
    }
    return entries;
}

Stations::Stations(const std::string &id, const std::string &title,
                   const std::string &country) {
    this->id = id;
    this->title = title;
    this->country = country;
    this->built = false;
    this->selected_station = 0;
}

Stations::Stations(const std::string &id) {
    this->id = id;
    this->built = false;
    this->selected_station = 0;
}

void Stations::getStationJson() {
    std::string buffer;
    std::string gardenRadio =
        "https://radio.garden/api/ara/content/page/" + this->id;
    this->httpClient.get(gardenRadio, buffer, false, "");
    try {
        this->jsonData = nlohmann::json::parse(buffer);
    } catch (nlohmann::json::parse_error &e) {
        std::cout << "Error: " << e.what() << std::endl;
    }
}

void Stations::buildStations() {
    this->getStationJson();
    if (this->jsonData.contains("data") &&
        this->jsonData["data"].contains("content") &&
        !this->jsonData["data"]["content"].empty()) {
        this->title = this->jsonData["data"]["content"][0]["title"];
    }

    auto entries = parseRadiosFromPageJson(this->jsonData, this->id, this->title,
                                           this->country);
    for (const auto &entry : entries) {
        this->radios.push_back(Radio(entry.radio_title, entry.href));
    }
    this->built = true;
}

void Stations::print() const {
    if (!this->built) {
        std::cout << "[*] buildStations() not called yet." << std::endl;
    } else {
        std::cout << "Stations found: " << this->radios.size() << std::endl;
        for (size_t i = 0; i < this->radios.size(); i++) {
            std::cout << "[" << i << "] ";
            this->radios.at(i).print();
        }
    }
}

void Stations::selectRadio() {
    if (!this->built) {
        std::cout << "[*] buildStations() not called yet." << std::endl;
    } else {
        this->print();
        std::cout << "Select a station: ";
        std::cin >> this->selected_station;
    }
}

void Stations::playRadio() {
    if (!this->built) {
        std::cout << "[*] buildStations() not called yet." << std::endl;
    } else {
        Radio radio = this->radios.at(this->selected_station);
        std::string cmd = "mpv --no-video --http-header-fields='Referer: https://radio.garden/' " + radio.buildUrl();
        system(cmd.c_str());
    }
}
