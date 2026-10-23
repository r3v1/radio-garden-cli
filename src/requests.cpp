#include "requests.hpp"

#include <curl/curl.h>
#include <fstream>
#include <iostream>

namespace {
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
const CurlGlobal &curlGlobal() {
    static CurlGlobal instance;
    return instance;
}
} // namespace

Http::Http() { (void)curlGlobal(); }

Http::~Http() {}

bool Http::get(const std::string &url, std::string &buffer, bool saveJson,
               const std::string &filename) {
    CURL *curl;
    CURLcode res;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "charset: utf-8");

    curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[!] HTTP request failed: " << curl_easy_strerror(res)
                  << std::endl;
        return false;
    }

    if (buffer.empty()) {
        std::cerr << "[!] No data received from the HTTP request" << std::endl;
        return false;
    }

    if (saveJson) {
        toJson(buffer, filename);
    }

    return true;
}

size_t Http::writeCallback(char *in, size_t size, size_t num,
                           std::string *out) {
    size_t totalBytes(size * num);
    out->append(in, totalBytes);
    return totalBytes;
}

void Http::toJson(const std::string &buffer, const std::string &filename) {
    std::ofstream jsonFile(filename);
    jsonFile << buffer;
    jsonFile.close();
}
