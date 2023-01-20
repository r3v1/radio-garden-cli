#include <cstring>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <string>

/*
 * Function to handle the data received from the HTTP request
 * in - pointer to the incoming data
 * size - size of each element of incoming data
 * num - number of elements in incoming data
 * out - pointer to the string buffer where the data will be stored
 */
size_t writeCallback(char *in, size_t size, size_t num, std::string *out) {
    // Calculate the total number of bytes in incoming data
    size_t totalBytes(size * num);

    // Append the incoming data to the buffer
    out->append(in, totalBytes);

    // Return the total number of bytes received
    return totalBytes;
}

void toJson(std::string buffer, std::string filename) {
    // Open a file stream to write the received data to a file
    std::ofstream jsonFile(filename);

    // Write the received data to the file
    jsonFile << buffer;

    // Close the file stream
    jsonFile.close();
}

void get(const std::string url, std::string &buffer, bool saveJson,
         std::string filename) {
    // std::cout << "Performing HTTP request: " << url << std::endl;

    CURL *curl;
    CURLcode res;

    struct curl_slist *headers = NULL; // init to NULL is important
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "charset: utf-8");

    // Initialize the cURL library
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects

        // Set the URL for the HTTP request
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // Set the callback function to handle the data received from the HTTP
        // request
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);

        // Set the pointer to the buffer as the data object to be passed to the
        // callback function
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

        // Perform the HTTP request
        res = curl_easy_perform(curl);

        // Check for errors
        if (res != CURLE_OK) {
            std::cout << "[!] HTTP request failed: " << curl_easy_strerror(res)
                      << std::endl;
        }

        // Cleanup the cURL library
        curl_easy_cleanup(curl);
    }

    if (buffer.empty()) {
        std::cout << "[!] No data received from the HTTP request" << std::endl;
    }

    if (saveJson) {
        // Convert the received data to JSON format
        toJson(buffer, filename);
    }
}
