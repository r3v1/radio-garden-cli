#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace search_detail {

inline bool isAsciiAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

inline bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// U+00C0..U+00FF folded to ASCII.
inline const char *foldLatin1(uint32_t cp) {
    static const char *const table[64] = {
        "a",  "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e",
        "e",  "i", "i", "i", "i", "d", "n",  "o", "o", "o", "o",
        "o",  "x", "o", "u", "u", "u", "u",  "y", "th", "ss", "a",
        "a",  "a", "a", "a", "a", "ae", "c", "e", "e", "e",  "e",
        "i",  "i", "i", "i", "d", "n", "o",  "o", "o", "o",  "o",
        "/",  "o", "u", "u", "u", "u", "y",  "th", "y"};
    return table[cp - 0xC0];
}

// U+0100..U+017F folded to ASCII.
inline char foldLatinExtA(uint32_t cp) {
    static const char table[] =
        "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"
        "IiIiIiIiIi" "Ii" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo"
        "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";
    return table[cp - 0x100];
}

} // namespace search_detail

// Lowercases ASCII and folds Latin letters with diacritics ("Málaga" ->
// "malaga", "España" -> "espana"). Other scripts are copied unchanged.
inline std::string normalizeForSearch(const std::string &input) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            out += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            i++;
            continue;
        }

        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (len == 1 || i + len > input.size()) {
            i++;
            continue;
        }
        uint32_t cp = c & (0xFFu >> (len + 1));
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char b = static_cast<unsigned char>(input[i + k]);
            if ((b & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!valid) {
            i++;
            continue;
        }

        if (cp >= 0xC0 && cp <= 0xFF) {
            out += search_detail::foldLatin1(cp);
        } else if (cp >= 0x100 && cp <= 0x17F) {
            char folded = search_detail::foldLatinExtA(cp);
            out += static_cast<char>(folded >= 'A' && folded <= 'Z'
                                         ? folded + 32
                                         : folded);
        } else {
            out.append(input, i, len);
        }
        i += len;
    }
    return out;
}

inline bool isSubsequence(const std::string &query, const std::string &text) {
    size_t qi = 0;
    for (char c : text) {
        if (qi < query.size() && c == query[qi]) {
            qi++;
        }
    }
    return qi == query.size();
}

// True if `query` is a subsequence of a single word of `text` (a run of
// alphanumerics or non-ASCII bytes): "bilbo" matches "bilbao" but not letters
// scattered over a whole phrase.
inline bool isWordSubsequence(const std::string &query,
                              const std::string &text) {
    size_t i = 0;
    while (i < text.size()) {
        auto inWord = [&](char c) {
            return search_detail::isAsciiAlnum(c) ||
                   (static_cast<unsigned char>(c) >= 0x80);
        };
        while (i < text.size() && !inWord(text[i])) {
            i++;
        }
        size_t qi = 0;
        while (i < text.size() && inWord(text[i])) {
            if (qi < query.size() && text[i] == query[qi]) {
                qi++;
            }
            i++;
        }
        if (qi == query.size()) {
            return true;
        }
    }
    return false;
}

// Whitespace-separated query tokens, matched against two fields (a primary
// one such as the radio title and a secondary one such as the place). Lower
// scores rank first; -1 means no match. Inputs must already be normalized.
class QueryMatcher {
  public:
    explicit QueryMatcher(const std::string &query) {
        std::string norm = normalizeForSearch(query);
        size_t i = 0;
        while (i < norm.size()) {
            while (i < norm.size() && search_detail::isAsciiSpace(norm[i])) {
                i++;
            }
            size_t j = i;
            while (j < norm.size() && !search_detail::isAsciiSpace(norm[j])) {
                j++;
            }
            if (j > i) {
                tokens_.push_back(norm.substr(i, j - i));
            }
            i = j;
        }
    }

    bool empty() const { return tokens_.empty(); }

    bool canFuzzy() const {
        return std::any_of(tokens_.begin(), tokens_.end(),
                           [](const std::string &t) { return t.size() >= 3; });
    }

    int score(const std::string &primary, const std::string &secondary) const {
        int total = 0;
        for (const auto &token : tokens_) {
            int p = substringCost(primary, token);
            if (p >= 0) {
                total += p;
                continue;
            }
            int s = substringCost(secondary, token);
            if (s < 0) {
                return -1;
            }
            total += 100 + s;
        }
        return total;
    }

    // Like score(), but a token may also match as a subsequence within one
    // word ("bilbo" -> "bilbao"). Only worth running when score() found nothing.
    int fuzzyScore(const std::string &primary,
                   const std::string &secondary) const {
        int total = 0;
        for (const auto &token : tokens_) {
            int p = substringCost(primary, token);
            if (p >= 0) {
                total += p;
                continue;
            }
            int s = substringCost(secondary, token);
            if (s >= 0) {
                total += 100 + s;
                continue;
            }
            if (token.size() < 3) {
                return -1;
            }
            if (isWordSubsequence(token, primary)) {
                total += 500;
            } else if (isWordSubsequence(token, secondary)) {
                total += 600;
            } else {
                return -1;
            }
        }
        return total;
    }

  private:
    static int substringCost(const std::string &text, const std::string &tok) {
        size_t pos = text.find(tok);
        if (pos == std::string::npos) {
            return -1;
        }
        int best = 1 << 20;
        while (pos != std::string::npos) {
            int cost;
            if (pos == 0) {
                cost = text.size() == tok.size() ? 0 : 5;
            } else if (!search_detail::isAsciiAlnum(text[pos - 1])) {
                cost = 15 + static_cast<int>(std::min<size_t>(pos, 64)) / 8;
            } else {
                cost = 40 + static_cast<int>(std::min<size_t>(pos, 64)) / 8;
            }
            best = std::min(best, cost);
            if (best == 0 || best == 5) {
                break;
            }
            pos = text.find(tok, pos + 1);
        }
        return best;
    }

    std::vector<std::string> tokens_;
};
