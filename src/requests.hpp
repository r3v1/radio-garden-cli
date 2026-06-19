#pragma once

#include <string>

class Http {
  public:
    Http();
    ~Http();

    bool get(const std::string &url, std::string &buffer, bool saveJson,
             const std::string &filename);

  private:
    static size_t writeCallback(char *in, size_t size, size_t num,
                                std::string *out);
    static void toJson(const std::string &buffer, const std::string &filename);
};
