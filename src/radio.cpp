#include "requests.cpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

class Radio {
  public:
    std::string href;
    std::string title;
    std::string id;

    Radio(std::string title, std::string href) {
        this->href = href;
        this->title = title;
        this->id = this->extractId(href);
    }

    void print() { std::cout << this->title << std::endl; }

    static std::string extractId(std::string href) {
        std::string delimiter = "/";
        std::string id = href.substr(href.find_last_of(delimiter) + 1);
        return id;
    }

    std::string buildUrl() {
        // Current epoch time
        std::time_t now = std::time(0);
        std::string epoch = std::to_string(now);

        std::string url = "https://radio.garden/api/ara/content/listen/" +
                          this->id + "/channel.mp3?" + epoch;
        return url;
    }
    
    void openAudioStream() {
        std::string url = this->buildUrl();

        std::string cmd = "mpv --no-video " + url;
        system(cmd.c_str());
    }
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

    Stations(std::string id, std::string title, std::string country) {
        this->id = id;
        this->title = title;
        this->built = false;
        this->country = country;
        this->selected_station = 0;
    }

    Stations(std::string id) {
        this->id = id;
        this->built = false;
        this->selected_station = 0;
    }

    void getStationJson() {
        std::string buffer;
        std::string gardenRadio =
            "https://radio.garden/api/ara/content/page/" + this->id;

        // Gets radio
        this->httpClient.get(gardenRadio, buffer, false, "radio.json");

        // Parses the JSON
        try {
            this->jsonData = nlohmann::json::parse(buffer);
        } catch (nlohmann::json::parse_error &e) {
            std::cout << "Error: " << e.what() << std::endl;
        }
    }

    void buildStations() {
        this->getStationJson();
        this->title = this->jsonData["data"]["content"][0]["title"];

        for (auto content : this->jsonData["data"]["content"]) {
            for (auto element : content["items"]) {
                if (element.contains("page")) {
                    auto page= element["page"];
                    if (page.contains("url")) {
                        std::string url = page["url"];
                        std::string title = page["title"];

                        Radio radio = Radio(title, url);
                        this->radios.push_back(radio);
                    }
                }
            }
        }
        this->built = true;
    }

    void print() {
        if (!this->built) {
            std::cout << "[*] buildStations() not called yet." << std::endl;
        } else {
            std::cout << "Stations found: " << this->radios.size() << std::endl;
            for (int i = 0; i < this->radios.size(); i++) {
                Radio radio = this->radios.at(i);
                std::cout << "[" << i << "] ";
                radio.print();
            }
        }
    }

    void selectRadio() {
        if (!this->built) {
            std::cout << "[*] buildStations() not called yet." << std::endl;
        } else {
            this->print();
            std::cout << "Select a station: ";
            std::cin >> this->selected_station;
        }
    }

    void playRadio() {
        if (!this->built) {
            std::cout << "[*] buildStations() not called yet." << std::endl;
        } else {
            Radio radio = this->radios.at(this->selected_station);
            radio.openAudioStream();
        }
    }
};

// int main(int argc, char *argv[]) {
//     if (argc < 2) {
//         std::cout << "Usage: " << argv[0] << " <station_id>" << std::endl;
//         return 1;
//     }

//     std::string id = argv[1];
//     // std::string id = "DFYIUBpn";
//     // std::string id = "GBrGVmKv";
//     Stations stations = Stations(id);
//     stations.buildStations();
//     stations.selectRadio();
// }