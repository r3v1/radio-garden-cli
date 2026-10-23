#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "radio.hpp"
#include "requests.hpp"

nlohmann::json loadPlacesJson();
std::vector<Place> getPlacesByCountry(const std::string &country);
std::vector<RadioEntry> fetchPlaceRadios(const std::string &placeId,
                                         const std::string &placeTitle,
                                         const std::string &country);

struct SearchRow {
    RadioEntry entry;
    std::string norm_radio;
    std::string norm_place;
    uint64_t rank = 0; // lower wins when a radio is listed on several places
};

struct PlaceHit {
    int score;
    Place place;
};

// In-memory index of every radio in a country. A pool of background workers
// fills it from the per-place disk cache (or the network); searches run
// synchronously on the caller's thread and only hold the lock for the scan.
//
// Radio Garden lists the same radio on several place pages, so the search
// corpus holds each radio once (attributed to the best-fitting place), while
// the per-place lists keep every page exactly as the API returned it.
class RadioIndex {
  public:
    explicit RadioIndex(const std::string &country);
    ~RadioIndex();

    void startBackgroundBuild();
    void stopBackgroundBuild();

    size_t corpusSize() const { return corpus_size_; }
    // Bumped every time radios are added to the corpus.
    uint64_t version() const { return version_; }

    int buildProgress() const { return progress_; }
    int buildTotal() const { return static_cast<int>(places_.size()); }
    bool isBuilding() const { return building_; }

    const std::vector<Place> &places() const { return places_; }
    // Radios listed on the place's page, in page order.
    size_t placeRadioCount(const std::string &placeId) const;

    std::vector<PlaceHit> searchPlaces(const std::string &query,
                                       size_t limit) const;
    std::vector<RadioEntry> searchRadios(const std::string &query,
                                         size_t limit) const;
    std::vector<RadioEntry> radiosInPlace(const std::string &placeId) const;

  private:
    void workerLoop();
    void publish(size_t placeIndex, const std::vector<RadioEntry> &radios);
    bool loadPlaceFromCache(const std::string &placeId,
                            std::vector<RadioEntry> &out) const;
    void savePlaceToCache(const std::string &placeId,
                          const std::vector<RadioEntry> &entries) const;

    std::string country_;
    std::vector<Place> places_;
    std::vector<std::string> norm_places_;
    std::unordered_map<std::string, size_t> place_index_;

    mutable std::mutex mutex_;
    std::vector<SearchRow> corpus_;
    std::unordered_map<std::string, uint32_t> row_by_radio_id_;
    std::vector<std::vector<RadioEntry>> place_radios_; // by place index

    std::vector<std::thread> workers_;
    std::atomic<size_t> next_place_{0};
    std::atomic<int> active_workers_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> building_{false};
    std::atomic<int> progress_{0};
    std::atomic<size_t> corpus_size_{0};
    std::atomic<uint64_t> version_{0};
};
