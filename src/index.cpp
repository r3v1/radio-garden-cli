#include "index.hpp"

#include "paths.hpp"
#include "search_utils.hpp"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <set>
#include <unordered_set>

static constexpr size_t kMaxSearchResults = 400;

static SearchRow makeSearchRow(const RadioEntry &entry) {
    SearchRow row;
    row.entry = entry;
    row.norm_radio = normalizeForSearch(entry.radio_title);
    row.norm_place = normalizeForSearch(entry.place_title);
    row.norm_combined = row.norm_radio + " " + row.norm_place;
    return row;
}

nlohmann::json loadPlacesJson() {
    std::ifstream f(getPlacesPath());
    Http http;
    if (!f.good()) {
        std::string buffer;
        std::string url = "https://radio.garden/api/ara/content/places";
        http.get(url, buffer, true, getPlacesPath());
        return nlohmann::json::parse(buffer);
    }
    return nlohmann::json::parse(f);
}

std::vector<Place> getPlacesByCountry(const std::string &country) {
    std::vector<Place> places;
    auto json = loadPlacesJson();
    for (const auto &item : json["data"]["list"]) {
        if (item["country"].get<std::string>() == country) {
            Place p;
            p.id = item["id"].get<std::string>();
            p.title = item["title"].get<std::string>();
            p.country = country;
            places.push_back(p);
        }
    }
    std::sort(places.begin(), places.end(),
              [](const Place &a, const Place &b) { return a.title < b.title; });
    return places;
}

std::vector<RadioEntry> fetchPlaceRadios(const std::string &placeId,
                                         const std::string &placeTitle,
                                         const std::string &country) {
    std::ifstream cache(getPlaceCachePath(placeId));
    if (cache.good()) {
        try {
            nlohmann::json j = nlohmann::json::parse(cache);
            return j.get<std::vector<RadioEntry>>();
        } catch (...) {
        }
    }

    Http http;
    std::string buffer;
    std::string url = "https://radio.garden/api/ara/content/page/" + placeId;
    if (!http.get(url, buffer, false, "")) {
        return {};
    }

    try {
        nlohmann::json pageJson = nlohmann::json::parse(buffer);
        auto entries =
            parseRadiosFromPageJson(pageJson, placeId, placeTitle, country);
        std::ofstream out(getPlaceCachePath(placeId));
        out << nlohmann::json(entries).dump(2);
        return entries;
    } catch (...) {
        return {};
    }
}

RadioIndex::RadioIndex(const std::string &country) : country_(country) {
    loadPlacesJson();
    places_ = getPlacesByCountry(country);
    total_ = static_cast<int>(places_.size());
    loadIndexFromDisk();
    loadCorpusFromCache();
    search_worker_ = std::thread(&RadioIndex::searchWorkerLoop, this);
}

RadioIndex::~RadioIndex() {
    search_stop_ = true;
    search_cv_.notify_all();
    if (search_worker_.joinable()) {
        search_worker_.join();
    }
    stopBackgroundBuild();
}

void RadioIndex::loadIndexFromDisk() {
    std::ifstream f(getIndexPath(country_));
    if (!f.good()) {
        return;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(f);
        entries_ = j.get<std::vector<RadioEntry>>();
    } catch (...) {
        entries_.clear();
    }
}

void RadioIndex::saveIndexToDisk() const {
    std::ofstream f(getIndexPath(country_));
    f << nlohmann::json(entries_).dump(2);
}

void RadioIndex::appendToCorpusLocked(const std::vector<RadioEntry> &batch) {
    std::unordered_set<std::string> seen;
    for (const auto &row : corpus_) {
        seen.insert(row.entry.id);
    }
    for (const auto &entry : batch) {
        if (!entry.id.empty() && seen.insert(entry.id).second) {
            corpus_.push_back(makeSearchRow(entry));
        }
    }
}

void RadioIndex::appendToCorpus(const std::vector<RadioEntry> &batch) {
    if (batch.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    appendToCorpusLocked(batch);
}

void RadioIndex::loadCorpusFromCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    corpus_.clear();
    appendToCorpusLocked(entries_);

    std::string dirPath = getCacheDir() + "/places";
    DIR *dir = opendir(dirPath.c_str());
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json") {
            continue;
        }

        std::ifstream cache(dirPath + "/" + name);
        if (!cache.good()) {
            continue;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(cache);
            auto radios = j.get<std::vector<RadioEntry>>();
            appendToCorpusLocked(radios);
        } catch (...) {
        }
    }
    closedir(dir);
}

bool RadioIndex::loadPlaceFromCache(const std::string &placeId,
                                    std::vector<RadioEntry> &out) const {
    std::ifstream cache(getPlaceCachePath(placeId));
    if (!cache.good()) {
        return false;
    }
    try {
        out = nlohmann::json::parse(cache).get<std::vector<RadioEntry>>();
        return true;
    } catch (...) {
        return false;
    }
}

void RadioIndex::savePlaceToCache(const std::string &placeId,
                                  const std::vector<RadioEntry> &entries) const {
    std::ofstream out(getPlaceCachePath(placeId));
    out << nlohmann::json(entries).dump(2);
}

void RadioIndex::startBackgroundBuild() {
    if (building_) {
        return;
    }
    stop_ = false;
    building_ = true;
    worker_ = std::thread(&RadioIndex::buildLoop, this);
}

void RadioIndex::stopBackgroundBuild() {
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    building_ = false;
}

void RadioIndex::buildLoop() {
    std::set<std::string> indexedPlaces;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &e : entries_) {
            indexedPlaces.insert(e.place_id);
        }
        progress_ = static_cast<int>(indexedPlaces.size());
    }

    for (const auto &place : places_) {
        if (stop_) {
            break;
        }
        if (indexedPlaces.count(place.id)) {
            continue;
        }

        std::vector<RadioEntry> radios;
        if (!loadPlaceFromCache(place.id, radios)) {
            Http http;
            std::string buffer;
            std::string url =
                "https://radio.garden/api/ara/content/page/" + place.id;
            if (http.get(url, buffer, false, "")) {
                try {
                    nlohmann::json pageJson = nlohmann::json::parse(buffer);
                    radios = parseRadiosFromPageJson(pageJson, place.id,
                                                     place.title, country_);
                    savePlaceToCache(place.id, radios);
                } catch (...) {
                }
            }
        }

        if (!radios.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.insert(entries_.end(), radios.begin(), radios.end());
            appendToCorpusLocked(radios);
            saveIndexToDisk();
        }

        progress_ = static_cast<int>(indexedPlaces.size()) + 1;
        indexedPlaces.insert(place.id);
    }

    building_ = false;
}

std::vector<RadioEntry> RadioIndex::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::vector<Place> RadioIndex::places() const { return places_; }

size_t RadioIndex::corpusSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return corpus_.size();
}

int RadioIndex::buildProgress() const { return progress_; }

int RadioIndex::buildTotal() const { return total_; }

bool RadioIndex::isBuilding() const { return building_; }

std::vector<Place> RadioIndex::searchPlaces(const std::string &query) const {
    if (query.empty()) {
        return {};
    }
    std::string normQuery = normalizeForSearch(query);
    std::vector<std::pair<int, Place>> scored;
    for (const auto &p : places_) {
        std::string normTitle = normalizeForSearch(p.title);
        if (!matchesNormalized(normTitle, normQuery)) {
            continue;
        }
        scored.push_back({matchScore(p.title, query), p});
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<Place> result;
    result.reserve(scored.size());
    for (const auto &item : scored) {
        result.push_back(item.second);
    }
    return result;
}

std::vector<RadioEntry>
RadioIndex::searchCorpus(const std::string &query) const {
    if (query.empty()) {
        return {};
    }

    std::string normQuery = normalizeForSearch(query);
    std::vector<std::pair<int, RadioEntry>> scored;

    std::lock_guard<std::mutex> lock(mutex_);
    scored.reserve(std::min(corpus_.size(), kMaxSearchResults));

    for (const auto &row : corpus_) {
        if (!matchesNormalized(row.norm_combined, normQuery) &&
            !matchesNormalized(row.norm_place, normQuery)) {
            continue;
        }
        int score = std::min(matchScore(row.entry.radio_title, query),
                             matchScore(row.entry.place_title, query));
        scored.push_back({score, row.entry});
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    if (scored.size() > kMaxSearchResults) {
        scored.resize(kMaxSearchResults);
    }

    std::vector<RadioEntry> results;
    results.reserve(scored.size());
    for (const auto &item : scored) {
        results.push_back(item.second);
    }
    return results;
}

void RadioIndex::requestSearch(const std::string &query) {
    {
        std::lock_guard<std::mutex> lock(search_mutex_);
        pending_query_ = query;
        search_dirty_ = true;
    }
    search_cv_.notify_one();
}

bool RadioIndex::isSearchPending() const { return search_dirty_; }

bool RadioIndex::hasFreshSearchResults(const std::string &query) const {
    std::lock_guard<std::mutex> lock(search_mutex_);
    return !search_dirty_ && completed_query_ == query;
}

std::vector<RadioEntry> RadioIndex::getSearchResults() const {
    std::lock_guard<std::mutex> lock(search_mutex_);
    return search_results_;
}

void RadioIndex::searchWorkerLoop() {
    while (!search_stop_) {
        std::string query;
        {
            std::unique_lock<std::mutex> lock(search_mutex_);
            search_cv_.wait_for(lock, std::chrono::milliseconds(35), [&] {
                return search_dirty_ || search_stop_;
            });
            if (search_stop_) {
                break;
            }
            if (!search_dirty_) {
                continue;
            }
            query = pending_query_;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        {
            std::unique_lock<std::mutex> lock(search_mutex_);
            if (search_stop_) {
                break;
            }
            if (pending_query_ != query) {
                continue;
            }
            query = pending_query_;
        }

        auto radios = searchCorpus(query);

        {
            std::lock_guard<std::mutex> lock(search_mutex_);
            if (pending_query_ != query) {
                continue;
            }
            search_results_ = std::move(radios);
            completed_query_ = query;
            search_dirty_ = false;
        }
    }
}
