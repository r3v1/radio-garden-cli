#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
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
    std::string norm_combined;
};

class RadioIndex {
  public:
    explicit RadioIndex(const std::string &country);
    ~RadioIndex();

    void startBackgroundBuild();
    void stopBackgroundBuild();

    std::vector<RadioEntry> entries() const;
    std::vector<Place> places() const;
    size_t corpusSize() const;

    int buildProgress() const;
    int buildTotal() const;
    bool isBuilding() const;

    std::vector<Place> searchPlaces(const std::string &query) const;

    void requestSearch(const std::string &query);
    bool isSearchPending() const;
    bool hasFreshSearchResults(const std::string &query) const;
    std::vector<RadioEntry> getSearchResults() const;

  private:
    void buildLoop();
    void loadIndexFromDisk();
    void saveIndexToDisk() const;
    void loadCorpusFromCache();
    void appendToCorpus(const std::vector<RadioEntry> &batch);
    void appendToCorpusLocked(const std::vector<RadioEntry> &batch);
    bool loadPlaceFromCache(const std::string &placeId,
                            std::vector<RadioEntry> &out) const;
    void savePlaceToCache(const std::string &placeId,
                          const std::vector<RadioEntry> &entries) const;
    std::vector<RadioEntry> searchCorpus(const std::string &query) const;
    void searchWorkerLoop();

    std::string country_;
    std::vector<Place> places_;
    std::vector<RadioEntry> entries_;
    std::vector<SearchRow> corpus_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::thread search_worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> building_{false};
    std::atomic<int> progress_{0};
    int total_{0};

    mutable std::mutex search_mutex_;
    std::condition_variable search_cv_;
    std::string pending_query_;
    std::string completed_query_;
    std::vector<RadioEntry> search_results_;
    std::atomic<bool> search_dirty_{false};
    std::atomic<bool> search_stop_{false};
};
