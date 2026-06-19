#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

inline std::string normalizeForSearch(const std::string &input) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        if (c < 128) {
            out += static_cast<char>(std::tolower(c));
            i++;
            continue;
        }

        if (i + 1 < input.size() && c == 0xC3) {
            unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
            switch (c2) {
            case 0x80:
            case 0xA0:
                out += 'a';
                break;
            case 0x81:
            case 0xA1:
                out += 'a';
                break;
            case 0x82:
            case 0xA2:
                out += 'a';
                break;
            case 0x83:
            case 0xA3:
                out += 'a';
                break;
            case 0x88:
            case 0xA8:
                out += 'e';
                break;
            case 0x89:
            case 0xA9:
                out += 'e';
                break;
            case 0x8A:
            case 0xAA:
                out += 'e';
                break;
            case 0x8B:
            case 0xAB:
                out += 'e';
                break;
            case 0x8C:
            case 0xAC:
                out += 'i';
                break;
            case 0x8D:
            case 0xAD:
                out += 'i';
                break;
            case 0x8E:
            case 0xAE:
                out += 'i';
                break;
            case 0x8F:
            case 0xAF:
                out += 'i';
                break;
            case 0x91:
            case 0xB1:
                out += 'n';
                break;
            case 0x92:
            case 0xB2:
                out += 'o';
                break;
            case 0x93:
            case 0xB3:
                out += 'o';
                break;
            case 0x94:
            case 0xB4:
                out += 'o';
                break;
            case 0x95:
            case 0xB5:
                out += 'o';
                break;
            case 0x96:
            case 0xB6:
                out += 'o';
                break;
            case 0x97:
            case 0xB7:
                out += 'o';
                break;
            case 0x99:
            case 0xB9:
                out += 'u';
                break;
            case 0x9A:
            case 0xBA:
                out += 'u';
                break;
            case 0x9B:
            case 0xBB:
                out += 'u';
                break;
            case 0x9C:
            case 0xBC:
                out += 'u';
                break;
            case 0x9D:
            case 0xBD:
                out += 'u';
                break;
            case 0x9E:
            case 0xBE:
                out += 'p';
                break;
            case 0x9F:
            case 0xBF:
                out += 'y';
                break;
            default:
                out += static_cast<char>(std::tolower(c2));
                break;
            }
            i += 2;
            continue;
        }

        out += static_cast<char>(std::tolower(c));
        i++;
    }

    return out;
}

inline bool isSubsequence(const std::string &query, const std::string &text) {
    if (query.empty()) {
        return true;
    }
    size_t qi = 0;
    for (unsigned char c : text) {
        if (qi < query.size() && c == query[qi]) {
            qi++;
        }
    }
    return qi == query.size();
}

inline bool wordMatchesQuery(const std::string &normText,
                             const std::string &normQuery) {
    if (normQuery.empty()) {
        return true;
    }

    size_t start = 0;
    while (start < normText.size()) {
        while (start < normText.size() && !std::isalnum(normText[start])) {
            start++;
        }
        if (start >= normText.size()) {
            break;
        }

        size_t end = start;
        while (end < normText.size() && std::isalnum(normText[end])) {
            end++;
        }

        std::string word = normText.substr(start, end - start);
        if (word.find(normQuery) != std::string::npos ||
            (normQuery.size() >= 3 && isSubsequence(normQuery, word)) ||
            (word.size() >= normQuery.size() &&
             word.compare(0, normQuery.size(), normQuery) == 0)) {
            return true;
        }
        start = end;
    }
    return false;
}

inline std::vector<std::string> searchTokens(const std::string &query) {
    std::vector<std::string> tokens;
    std::string norm = normalizeForSearch(query);
    std::istringstream stream(norm);
    std::string token;
    while (stream >> token) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    if (tokens.empty() && !norm.empty()) {
        tokens.push_back(norm);
    }
    return tokens;
}

inline bool matchesNormalized(const std::string &normText,
                              const std::string &normQuery) {
    if (normQuery.empty()) {
        return true;
    }
    if (normText.find(normQuery) != std::string::npos) {
        return true;
    }
    if (normQuery.size() >= 3 && isSubsequence(normQuery, normText)) {
        return true;
    }
    return wordMatchesQuery(normText, normQuery);
}

inline bool matchesQuery(const std::string &text, const std::string &query) {
    if (query.empty()) {
        return true;
    }

    std::string normText = normalizeForSearch(text);
    std::string normQuery = normalizeForSearch(query);

    if (normQuery.empty()) {
        return true;
    }

    if (normText.find(normQuery) != std::string::npos) {
        return true;
    }

    if (normText.size() >= normQuery.size() &&
        normText.compare(0, normQuery.size(), normQuery) == 0) {
        return true;
    }

    if (normQuery.size() >= 3 && isSubsequence(normQuery, normText)) {
        return true;
    }

    if (wordMatchesQuery(normText, normQuery)) {
        return true;
    }

    auto tokens = searchTokens(normQuery);
    if (tokens.empty()) {
        return false;
    }

    for (const auto &token : tokens) {
        if (token.size() < 2) {
            if (normText.find(token) == std::string::npos) {
                return false;
            }
            continue;
        }
        if (normText.find(token) == std::string::npos &&
            !isSubsequence(token, normText) && !wordMatchesQuery(normText, token)) {
            return false;
        }
    }
    return true;
}

inline int matchScore(const std::string &text, const std::string &query) {
    std::string normText = normalizeForSearch(text);
    std::string normQuery = normalizeForSearch(query);

    if (normQuery.empty()) {
        return 1000;
    }
    if (normText == normQuery) {
        return 0;
    }
    if (normText.find(normQuery) == 0) {
        return 10;
    }
    if (normText.find(normQuery) != std::string::npos) {
        return 50;
    }

    auto tokens = searchTokens(normQuery);
    bool allFound = true;
    int earliest = static_cast<int>(normText.size());
    for (const auto &token : tokens) {
        size_t pos = normText.find(token);
        if (pos == std::string::npos) {
            allFound = false;
            break;
        }
        earliest = std::min(earliest, static_cast<int>(pos));
    }
    if (allFound) {
        return 100 + earliest;
    }
    return 10000;
}
