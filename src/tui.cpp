#include "radio.cpp"
#include <algorithm>
#include <cctype>
#include <clocale>
#include <fstream>
#include <iostream>
#include <ncurses.h>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

// Structure to hold country information
struct CountryInfo {
    std::string name;
    std::vector<Stations> stations;
};

// Get unique list of countries from places.json
std::vector<std::string> getUniqueCountries() {
    std::ifstream f("places.json");
    nlohmann::json jsonData;
    Http httpClient;
    
    if (!f.good()) {
        // Download the file if missing
        std::string buffer;
        std::string url = "https://radio.garden/api/ara/content/places";
        httpClient.get(url, buffer, true, "places.json");
        jsonData = nlohmann::json::parse(buffer);
    } else {
        jsonData = nlohmann::json::parse(f);
    }

    auto list = jsonData["data"]["list"];
    std::set<std::string> countriesSet;

    for (auto item : list) {
        std::string country = item["country"];
        if (!country.empty()) {
            countriesSet.insert(country);
        }
    }

    std::vector<std::string> countries(countriesSet.begin(), countriesSet.end());
    std::sort(countries.begin(), countries.end());
    
    return countries;
}

// Get stations for a specific country
std::vector<Stations> getStationsByCountry(const std::string &country) {
    std::ifstream f("places.json");
    nlohmann::json jsonData;
    Http httpClient;
    
    if (!f.good()) {
        std::string buffer;
        std::string url = "https://radio.garden/api/ara/content/places";
        httpClient.get(url, buffer, true, "places.json");
        jsonData = nlohmann::json::parse(buffer);
    } else {
        jsonData = nlohmann::json::parse(f);
    }

    auto list = jsonData["data"]["list"];
    std::vector<Stations> stationsList;

    for (auto item : list) {
        std::string itemCountry = item["country"];
        if (itemCountry == country) {
            std::string title = item["title"];
            std::string id = item["id"];
            Stations stations(id, title, country);
            stationsList.push_back(stations);
        }
    }

    return stationsList;
}

// Helper function to convert string to lowercase for case-insensitive search
std::string toLower(const std::string &str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// Display menu and return selected index
// ncurses should be initialized before calling this function
int showMenu(const std::vector<std::string> &items, const std::string &title) {
    // Enable UTF-8 support
    setlocale(LC_ALL, "");
    
    int selected = 0;
    int max_y, max_x;
    int start_y, start_x;
    
    getmaxyx(stdscr, max_y, max_x);
    
    // Calculate window position (centered)
    // Limit menu height to screen size
    int max_display_items = std::min((int)items.size(), max_y - 6);
    int menu_height = max_display_items + 4; // +4 for title and borders
    int menu_width = std::min(80, max_x - 4);
    start_y = (max_y - menu_height) / 2;
    start_x = (max_x - menu_width) / 2;
    
    // Create window for menu
    WINDOW *menu_win = newwin(menu_height, menu_width, start_y, start_x);
    box(menu_win, 0, 0);
    
    // Search functionality
    std::string searchQuery = "";
    std::vector<int> filteredIndices; // Original indices of filtered items
    bool isSearchMode = false;
    bool useFiltered = false;
    
    // Initialize filtered indices to all items
    for (size_t i = 0; i < items.size(); i++) {
        filteredIndices.push_back(i);
    }
    
    int ch;
    bool selected_item = false;
    int scroll_offset = 0;
    
    while (!selected_item) {
        // Handle search mode
        if (isSearchMode) {
            // Show search prompt
            clear();
            mvprintw(max_y - 1, 0, "/%s_", searchQuery.c_str());
            curs_set(1); // Show cursor in search mode
            refresh();
            
            ch = getch();
            
            if (ch == 27) { // ESC - cancel search
                isSearchMode = false;
                searchQuery = "";
                useFiltered = false;
                filteredIndices.clear();
                for (size_t i = 0; i < items.size(); i++) {
                    filteredIndices.push_back(i);
                }
                selected = 0;
                curs_set(0);
                continue;
            } else if (ch == 10 || ch == '\r') { // ENTER - exit search mode but keep filter
                isSearchMode = false;
                curs_set(0);
                continue;
            } else if (ch == 127 || ch == KEY_BACKSPACE) { // BACKSPACE
                if (!searchQuery.empty()) {
                    // Handle UTF-8 multi-byte character deletion
                    // Remove the last character (which might be multi-byte)
                    size_t len = searchQuery.length();
                    if (len > 0) {
                        // Find the start of the last UTF-8 character
                        size_t pos = len - 1;
                        // Skip continuation bytes (10xxxxxx)
                        while (pos > 0 && (searchQuery[pos] & 0xC0) == 0x80) {
                            pos--;
                        }
                        searchQuery.erase(pos);
                    }
                }
                // Apply filter immediately after backspace
                filteredIndices.clear();
                std::string lowerQuery = toLower(searchQuery);
                
                for (size_t i = 0; i < items.size(); i++) {
                    if (searchQuery.empty() || toLower(items[i]).find(lowerQuery) != std::string::npos) {
                        filteredIndices.push_back(i);
                    }
                }
                
                useFiltered = !searchQuery.empty();
                if (selected >= filteredIndices.size() && !filteredIndices.empty()) {
                    selected = filteredIndices.size() - 1;
                }
                if (selected < 0) {
                    selected = 0;
                }
                scroll_offset = 0; // Reset scroll when filtering changes
                continue;
            } else if (ch >= 32 && ch < 127) { // Printable ASCII characters
                searchQuery += (char)ch;
                // Apply filter immediately as user types (like vim)
                filteredIndices.clear();
                std::string lowerQuery = toLower(searchQuery);
                
                for (size_t i = 0; i < items.size(); i++) {
                    if (toLower(items[i]).find(lowerQuery) != std::string::npos) {
                        filteredIndices.push_back(i);
                    }
                }
                
                useFiltered = true;
                // Reset selection to first match
                selected = 0;
                scroll_offset = 0; // Reset scroll when filtering
                continue;
            } else if (ch >= 127) { // UTF-8 multi-byte characters
                // Handle UTF-8 characters (accents, etc.)
                searchQuery += (char)ch;
                
                // Read additional bytes for multi-byte UTF-8 characters
                // UTF-8 encoding: 110xxxxx 10xxxxxx (2 bytes)
                //                 1110xxxx 10xxxxxx 10xxxxxx (3 bytes)
                //                 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (4 bytes)
                int additional_bytes = 0;
                if ((ch & 0xE0) == 0xC0) additional_bytes = 1;      // 2-byte char
                else if ((ch & 0xF0) == 0xE0) additional_bytes = 2; // 3-byte char
                else if ((ch & 0xF8) == 0xF0) additional_bytes = 3; // 4-byte char
                
                for (int i = 0; i < additional_bytes; i++) {
                    int next_ch = getch();
                    if (next_ch != ERR) {
                        searchQuery += (char)next_ch;
                    }
                }
                
                // Apply filter
                filteredIndices.clear();
                std::string lowerQuery = toLower(searchQuery);
                
                for (size_t i = 0; i < items.size(); i++) {
                    if (toLower(items[i]).find(lowerQuery) != std::string::npos) {
                        filteredIndices.push_back(i);
                    }
                }
                
                useFiltered = true;
                selected = 0;
                scroll_offset = 0;
                continue;
            }
        }
        
        // Get current items list (filtered or all)
        const std::vector<std::string> *displayItems = &items;
        std::vector<std::string> filteredItems;
        
        if (useFiltered) {
            for (int idx : filteredIndices) {
                filteredItems.push_back(items[idx]);
            }
            displayItems = &filteredItems;
        }
        
        int items_count = displayItems->size();
        // Handle empty filtered results
        if (items_count == 0) {
            // Show "No results" message
            clear();
            werase(menu_win);
            delwin(menu_win);
            menu_win = newwin(menu_height, menu_width, start_y, start_x);
            box(menu_win, 0, 0);
            
            std::string noResultsMsg = "No results for: " + searchQuery;
            if (noResultsMsg.length() > menu_width - 4) {
                noResultsMsg = noResultsMsg.substr(0, menu_width - 7) + "...";
            }
            mvwprintw(menu_win, menu_height / 2, (menu_width - noResultsMsg.length()) / 2, "%s", noResultsMsg.c_str());
            mvwprintw(menu_win, menu_height - 2, 2, "Press ESC to clear filter, / to search again");
            
            refresh();
            wrefresh(menu_win);
            
            ch = getch();
            if (ch == 27) { // ESC - clear filter
                useFiltered = false;
                searchQuery = "";
                filteredIndices.clear();
                for (size_t i = 0; i < items.size(); i++) {
                    filteredIndices.push_back(i);
                }
                selected = 0;
                continue;
            } else if (ch == '/') {
                isSearchMode = true;
                searchQuery = "";
                continue;
            }
            continue;
        }
        
        // Recalculate menu dimensions based on filtered items
        // Menu structure: border(1) + title(1) + separator(1) + items(N) + instructions(1) + border(1)
        // Minimum needed: 1 + 1 + 1 + 1 + 1 + 1 = 6 lines for 1 item to be visible
        int min_height_for_one_item = 6; // border + title + separator + 1 item + instructions + border
        int available_height = max_y - 4; // Leave some margin at top/bottom
        // Calculate max items that can fit: available height minus title, separator, instructions, borders
        int max_items_fit = std::max(1, available_height - 4);
        max_display_items = std::min(items_count, max_items_fit);
        // Ensure menu height accommodates items + title + separator + instructions + borders
        // Formula: items + 4 (title + separator + instructions + borders)
        // Always ensure at least min_height_for_one_item
        menu_height = std::max(min_height_for_one_item, std::min(max_display_items + 4, available_height));
        start_y = std::max(0, (max_y - menu_height) / 2);
        
        // Ensure selected is within bounds
        if (selected >= items_count) {
            selected = items_count - 1;
        }
        if (selected < 0) {
            selected = 0;
        }
        
        // Clear screen and redraw menu
        clear();
        werase(menu_win);
        delwin(menu_win);
        menu_win = newwin(menu_height, menu_width, start_y, start_x);
        box(menu_win, 0, 0);
        
        // Print title (truncate if too long)
        std::string displayTitle = title;
        if (useFiltered && !searchQuery.empty()) {
            displayTitle += " (filtered: " + searchQuery + ")";
        }
        if (displayTitle.length() > menu_width - 4) {
            displayTitle = displayTitle.substr(0, menu_width - 7) + "...";
        }
        mvwprintw(menu_win, 1, (menu_width - displayTitle.length()) / 2, "%s", displayTitle.c_str());
        mvwprintw(menu_win, 2, 1, "%s", std::string(menu_width - 2, '-').c_str());
        
        // Calculate scroll offset to keep selected item visible
        // Ensure the selected item is always visible
        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + max_display_items) {
            scroll_offset = selected - max_display_items + 1;
        }
        // Ensure scroll_offset doesn't go negative
        if (scroll_offset < 0) {
            scroll_offset = 0;
        }
        
        // Calculate available space for items (excluding title, separator, instructions)
        // Title is at line 1, separator at line 2, instructions at menu_height-2
        // So items can be from line 3 to menu_height-3
        int items_start_y = 3;
        int items_end_y = menu_height - 3; // Leave space for instructions
        int available_items_lines = std::max(1, items_end_y - items_start_y + 1);
        int items_to_show = std::min(items_count, std::min(max_display_items, available_items_lines));
        
        // Print items (only visible ones)
        for (int i = 0; i < items_to_show && (i + scroll_offset) < items_count; i++) {
            int item_index = i + scroll_offset;
            int y_pos = items_start_y + i;
            
            // Ensure we don't overwrite instructions line or go beyond window
            if (y_pos > items_end_y || y_pos >= menu_height - 2) {
                break;
            }
            
            // Get the actual item text
            std::string item_text = (*displayItems)[item_index];
            
            // Truncate item text if too long (accounting for UTF-8 characters)
            int max_text_width = menu_width - 6;
            if (item_text.length() > max_text_width) {
                // Simple truncation - for better UTF-8 handling, would need more complex logic
                item_text = item_text.substr(0, max_text_width - 3) + "...";
            }
            
            if (item_index == selected) {
                wattron(menu_win, A_REVERSE);
                mvwprintw(menu_win, y_pos, 2, "> %s", item_text.c_str());
                wattroff(menu_win, A_REVERSE);
            } else {
                mvwprintw(menu_win, y_pos, 2, "  %s", item_text.c_str());
            }
        }
        
        // Print instructions (always at menu_height - 2)
        std::string instructions = "hjkl/↑↓: navigate, Ctrl+D/U: jump, /: search, ENTER: select, Q: quit";
        if (useFiltered && !searchQuery.empty()) {
            instructions += ", ESC: clear filter";
        }
        // Truncate instructions if too long
        if (instructions.length() > menu_width - 4) {
            instructions = instructions.substr(0, menu_width - 7) + "...";
        }
        mvwprintw(menu_win, menu_height - 2, 2, "%s", instructions.c_str());
        
        refresh();
        wrefresh(menu_win);
        
        ch = getch();
        
        // Calculate jump size (half page, similar to vim)
        int jump_size = std::max(1, max_display_items / 2);
        
        switch (ch) {
            case KEY_UP:
            case 'k':
            case 'K':
                selected--;
                if (selected < 0) {
                    selected = items_count - 1;
                }
                break;
            case KEY_DOWN:
            case 'j':
            case 'J':
                selected++;
                if (selected >= items_count) {
                    selected = 0;
                }
                break;
            case 4: // Ctrl+D (move down half page)
                selected += jump_size;
                if (selected >= items_count) {
                    selected = items_count - 1;
                }
                break;
            case 21: // Ctrl+U (move up half page)
                selected -= jump_size;
                if (selected < 0) {
                    selected = 0;
                }
                break;
            case '/':
                isSearchMode = true;
                searchQuery = "";
                break;
            case 10: // ENTER key (ASCII)
            case '\r': // Carriage return
                selected_item = true;
                break;
            case 'q':
            case 'Q':
                delwin(menu_win);
                curs_set(1);
                return -1; // User cancelled
                break;
            case 27: // ESC - clear filter if active
                if (useFiltered) {
                    useFiltered = false;
                    searchQuery = "";
                    filteredIndices.clear();
                    for (size_t i = 0; i < items.size(); i++) {
                        filteredIndices.push_back(i);
                    }
                    selected = 0;
                }
                break;
        }
    }
    
    delwin(menu_win);
    clear();
    refresh();
    curs_set(1);
    
    // Return the original index if filtered, otherwise return selected
    if (useFiltered && selected < filteredIndices.size()) {
        return filteredIndices[selected];
    }
    return selected;
}

// Main TUI function to select country, station, and radio, then play
void runTUI() {
    // Enable UTF-8 support before initializing ncurses
    setlocale(LC_ALL, "");
    
    // Initialize ncurses once for the entire TUI session
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0); // Hide cursor
    
    // Get list of countries
    std::vector<std::string> countries = getUniqueCountries();
    
    if (countries.empty()) {
        endwin();
        std::cerr << "No countries found!" << std::endl;
        exit(1);
    }
    
    // Show country selection menu
    int countryIndex = showMenu(countries, "Select a Country");
    
    if (countryIndex == -1) {
        endwin();
        std::cout << "Selection cancelled." << std::endl;
        exit(0);
    }
    
    std::string selectedCountry = countries[countryIndex];
    
    // Get stations for selected country
    std::vector<Stations> stations = getStationsByCountry(selectedCountry);
    
    if (stations.empty()) {
        endwin();
        std::cerr << "No stations found for " << selectedCountry << std::endl;
        exit(1);
    }
    
    // Create list of station titles for menu
    std::vector<std::string> stationTitles;
    for (const auto &station : stations) {
        stationTitles.push_back(station.title);
    }
    
    // Show station selection menu
    std::string stationTitle = "Select a Station in " + selectedCountry;
    int stationIndex = showMenu(stationTitles, stationTitle);
    
    if (stationIndex == -1) {
        endwin();
        std::cout << "Selection cancelled." << std::endl;
        exit(0);
    }
    
    // Show loading message
    clear();
    mvprintw(0, 0, "Loading radios for %s...", stations[stationIndex].title.c_str());
    refresh();
    
    // Get selected station and build its radios
    Stations selectedStation = stations[stationIndex];
    selectedStation.buildStations();
    
    if (selectedStation.radios.empty()) {
        endwin();
        std::cerr << "No radios found for " << selectedStation.title << std::endl;
        exit(1);
    }
    
    // Create list of radio titles for menu
    std::vector<std::string> radioTitles;
    for (const auto &radio : selectedStation.radios) {
        radioTitles.push_back(radio.title);
    }
    
    // Show radio selection menu
    std::string radioTitle = "Select a Radio in " + selectedStation.title;
    int radioIndex = showMenu(radioTitles, radioTitle);
    
    // Clean up ncurses before playing radio
    endwin();
    
    if (radioIndex == -1) {
        std::cout << "Selection cancelled." << std::endl;
        exit(0);
    }
    
    // Play selected radio
    selectedStation.selected_station = radioIndex;
    selectedStation.playRadio();
}
