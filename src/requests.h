#include <string>

#ifndef REQUESTS_H
#define REQUESTS_H

void get(const std::string url, std::string &buffer, bool saveJson,
         std::string filename = "");

#endif