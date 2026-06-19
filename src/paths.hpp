#pragma once

#include <string>

std::string getConfigDir();
std::string getCacheDir();
std::string getRuntimeDir();
std::string getStatePath();
std::string getPlacesPath();
std::string getMpvSocketPath();
std::string getIndexPath(const std::string &country);
std::string getPlaceCachePath(const std::string &placeId);
void ensureDir(const std::string &path);
