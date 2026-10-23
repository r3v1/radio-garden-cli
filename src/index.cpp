#include "index.hpp"

#include "paths.hpp"
#include "search_utils.hpp"

#include <algorithm>
#include <fstream>

static constexpr int kBuildWorkers = 4;

static SearchRow makeSearchRow(const RadioEntry &entry) {
    SearchRow row;
    row.entry = entry;
    row.norm_radio = normalizeForSearch(entry.radio_title);
    row.norm_place = normalizeForSearch(entry.place_title);
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
    std::stable_sort(places.begin(), places.end(),
                     [](const Place &a, const Place &b) {
                         return normalizeForSearch(a.title) <
                                normalizeForSearch(b.title);
                     });
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
    places_ = getPlacesByCountry(country);
    norm_places_.reserve(places_.size());
    for (size_t i = 0; i < places_.size(); ++i) {
        norm_places_.push_back(normalizeForSearch(places_[i].title));
        place_index_[places_[i].id] = i;
    }
    place_radios_.resize(places_.size());
}

RadioIndex::~RadioIndex() { stopBackgroundBuild(); }

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
    out << nlohmann::json(entries).dump();
}

void RadioIndex::startBackgroundBuild() {
    if (building_ || places_.empty()) {
        return;
    }
    stop_ = false;
    building_ = true;
    next_place_ = 0;
    progress_ = 0;
    active_workers_ = kBuildWorkers;
    for (int i = 0; i < kBuildWorkers; ++i) {
        workers_.emplace_back(&RadioIndex::workerLoop, this);
    }
}

void RadioIndex::stopBackgroundBuild() {
    stop_ = true;
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    building_ = false;
}

void RadioIndex::publish(size_t placeIndex,
                         const std::vector<RadioEntry> &radios) {
    if (radios.empty()) {
        return;
    }
    // A radio is attributed to the place whose name appears in its title
    // ("COPE Bilbao" -> Bilbao), otherwise to the first place alphabetically.
    // The rank makes the outcome independent of which worker finishes first.
    const std::string &normPlace = norm_places_[placeIndex];
    std::vector<SearchRow> rows;
    rows.reserve(radios.size());
    for (const auto &entry : radios) {
        if (entry.id.empty()) {
            continue;
        }
        SearchRow row = makeSearchRow(entry);
        bool titleMatch = normPlace.size() >= 3 &&
                          row.norm_radio.find(normPlace) != std::string::npos;
        row.rank = (titleMatch ? 0 : (uint64_t(1) << 32)) + placeIndex;
        rows.push_back(std::move(row));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    place_radios_[placeIndex] = radios;
    for (auto &row : rows) {
        auto it = row_by_radio_id_.find(row.entry.id);
        if (it == row_by_radio_id_.end()) {
            row_by_radio_id_[row.entry.id] =
                static_cast<uint32_t>(corpus_.size());
            corpus_.push_back(std::move(row));
        } else if (row.rank < corpus_[it->second].rank) {
            corpus_[it->second] = std::move(row);
        }
    }
    corpus_size_ = corpus_.size();
    version_++;
}

void RadioIndex::workerLoop() {
    Http http;
    while (!stop_) {
        size_t i = next_place_.fetch_add(1);
        if (i >= places_.size()) {
            break;
        }
        const Place &place = places_[i];

        std::vector<RadioEntry> radios;
        if (!loadPlaceFromCache(place.id, radios)) {
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

        publish(i, radios);
        progress_++;
    }
    if (--active_workers_ == 0) {
        building_ = false;
    }
}

size_t RadioIndex::placeRadioCount(const std::string &placeId) const {
    auto it = place_index_.find(placeId);
    if (it == place_index_.end()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return place_radios_[it->second].size();
}

std::vector<PlaceHit> RadioIndex::searchPlaces(const std::string &query,
                                               size_t limit) const {
    QueryMatcher matcher(query);
    std::vector<PlaceHit> hits;
    if (matcher.empty()) {
        return hits;
    }

    for (int pass = 0; pass < 2 && hits.empty(); ++pass) {
        if (pass == 1 && !matcher.canFuzzy()) {
            break;
        }
        for (size_t i = 0; i < places_.size(); ++i) {
            int score = pass == 0 ? matcher.score(norm_places_[i], "")
                                  : matcher.fuzzyScore(norm_places_[i], "");
            if (score >= 0) {
                hits.push_back({score, places_[i]});
            }
        }
    }

    std::stable_sort(
        hits.begin(), hits.end(),
        [](const PlaceHit &a, const PlaceHit &b) { return a.score < b.score; });
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

std::vector<RadioEntry> RadioIndex::searchRadios(const std::string &query,
                                                 size_t limit) const {
    QueryMatcher matcher(query);
    std::vector<RadioEntry> results;
    if (matcher.empty()) {
        return results;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int, uint32_t>> hits;
    for (int pass = 0; pass < 2 && hits.empty(); ++pass) {
        if (pass == 1 && !matcher.canFuzzy()) {
            break;
        }
        for (uint32_t i = 0; i < corpus_.size(); ++i) {
            const SearchRow &row = corpus_[i];
            int score = pass == 0
                            ? matcher.score(row.norm_radio, row.norm_place)
                            : matcher.fuzzyScore(row.norm_radio,
                                                 row.norm_place);
            if (score >= 0) {
                hits.push_back({score, i});
            }
        }
    }

    auto better = [&](const auto &a, const auto &b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        const std::string &ra = corpus_[a.second].norm_radio;
        const std::string &rb = corpus_[b.second].norm_radio;
        if (ra.size() != rb.size()) {
            return ra.size() < rb.size();
        }
        if (ra != rb) {
            return ra < rb;
        }
        // Corpus order depends on which worker finished first.
        return corpus_[a.second].entry.id < corpus_[b.second].entry.id;
    };
    size_t take = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + take, hits.end(), better);

    results.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        results.push_back(corpus_[hits[i].second].entry);
    }
    return results;
}

std::vector<RadioEntry>
RadioIndex::radiosInPlace(const std::string &placeId) const {
    auto it = place_index_.find(placeId);
    if (it == place_index_.end()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return place_radios_[it->second];
}
