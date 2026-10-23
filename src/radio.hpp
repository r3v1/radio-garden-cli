#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "requests.hpp"

struct RadioEntry {
    std::string radio_title;
    std::string place_id;
    std::string place_title;
    std::string href;
    std::string id;
    std::string country;
};

inline void to_json(nlohmann::json &j, const RadioEntry &e) {
    j = nlohmann::json{{"radio_title", e.radio_title},
                       {"place_id", e.place_id},
                       {"place_title", e.place_title},
                       {"href", e.href},
                       {"id", e.id},
                       {"country", e.country}};
}

inline void from_json(const nlohmann::json &j, RadioEntry &e) {
    e.radio_title = j.value("radio_title", "");
    e.place_id = j.value("place_id", "");
    e.place_title = j.value("place_title", "");
    e.href = j.value("href", "");
    e.id = j.value("id", "");
    e.country = j.value("country", "");
}

struct Place {
    std::string id;
    std::string title;
    std::string country;
};

class Radio {
  public:
    std::string href;
    std::string title;
    std::string id;

    Radio(const std::string &title, const std::string &href);

    void print() const;
    static std::string extractId(const std::string &href);
    std::string buildUrl() const;
};

class Stations {
  public:
    std::vector<Radio> radios;
    std::string id;
    std::string title;
    std::string country;
    nlohmann::json jsonData;
    bool built;
    int selected_station;
    Http httpClient;

    Stations(const std::string &id, const std::string &title,
             const std::string &country);
    explicit Stations(const std::string &id);

    void getStationJson();
    void buildStations();
    void print() const;
    void selectRadio();
    void playRadio();
};

// True for "/listen/..." hrefs; "/visit/..." ones are places, not radios.
bool isRadioHref(const std::string &href);
std::string radioBuildUrl(const std::string &radioId);
std::vector<RadioEntry>
parseRadiosFromPageJson(const nlohmann::json &json, const std::string &placeId,
                        const std::string &placeTitle,
                        const std::string &country);
