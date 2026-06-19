#pragma once

#include <atomic>
#include <string>

extern std::atomic<bool> g_mpris_quit;

void startMprisServer();
void stopMprisServer();
void notifyMprisMetadata(const std::string &title, const std::string &artist,
                         bool playing);
