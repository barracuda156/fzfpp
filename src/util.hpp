#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <atomic>

namespace fzf {

// Platform-safe string operations
inline std::vector<std::string> split_lines(const std::string& str) {
    std::vector<std::string> lines;
    std::string::size_type start = 0;
    std::string::size_type end;

    while ((end = str.find('\n', start)) != std::string::npos) {
        lines.push_back(str.substr(start, end - start));
        start = end + 1;
    }

    if (start < str.length()) {
        lines.push_back(str.substr(start));
    }

    return lines;
}

// Trim whitespace from string
inline std::string trim(const std::string& str) {
    auto start = std::find_if_not(str.begin(), str.end(), ::isspace);
    auto end = std::find_if_not(str.rbegin(), str.rend(), ::isspace).base();
    return (start < end) ? std::string(start, end) : std::string();
}

// Case conversion (ASCII-safe for now, UTF-8 handled by matcher)
inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

inline std::string to_lower_ascii(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](char c) { return to_lower_ascii(c); });
    return result;
}

// UTF-8 utilities (basic validation)
inline bool is_utf8_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

inline size_t utf8_char_length(char first_byte) {
    unsigned char c = static_cast<unsigned char>(first_byte);
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // Invalid, treat as single byte
}

// Clamp value between min and max
template<typename T>
inline T clamp(T value, T min_val, T max_val) {
    return std::max(min_val, std::min(value, max_val));
}

// Strip ANSI escape codes from a string
inline std::string strip_ansi_codes(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    bool in_escape = false;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\x1b' || str[i] == '\x9b') {
            // Start of escape sequence (ESC [ or CSI)
            in_escape = true;
        } else if (in_escape) {
            // End of escape sequence: letter character
            if ((str[i] >= 'A' && str[i] <= 'Z') ||
                (str[i] >= 'a' && str[i] <= 'z')) {
                in_escape = false;
            }
            // Continue skipping escape sequence characters
        } else {
            // Normal character, keep it
            result += str[i];
        }
    }

    return result;
}

// Colored text segment
struct ColoredSegment {
    std::string text;

    // Foreground color
    int fg_color;      // -1 = default, 0-15 = basic colors, 16-255 = 256 colors
    bool fg_is_rgb;    // True if fg_rgb is used instead of fg_color
    uint8_t fg_rgb[3]; // RGB values for true color mode

    // Background color
    int bg_color;      // -1 = default, 0-15 = basic colors, 16-255 = 256 colors
    bool bg_is_rgb;    // True if bg_rgb is used instead of bg_color
    uint8_t bg_rgb[3]; // RGB values for true color mode

    bool bold;
    bool reset;

    ColoredSegment() : fg_color(-1), fg_is_rgb(false), bg_color(-1), bg_is_rgb(false), bold(false), reset(false) {
        fg_rgb[0] = fg_rgb[1] = fg_rgb[2] = 0;
        bg_rgb[0] = bg_rgb[1] = bg_rgb[2] = 0;
    }
};

// Parse ANSI escape codes and extract colored segments
inline std::vector<ColoredSegment> parse_ansi_colored_text(const std::string& str) {
    std::vector<ColoredSegment> segments;
    ColoredSegment current;

    size_t i = 0;
    while (i < str.size()) {
        if (str[i] == '\x1b' && i + 1 < str.size() && str[i + 1] == '[') {
            // Save current segment if it has text
            if (!current.text.empty()) {
                segments.push_back(current);
                current.text.clear();
            }

            // Parse escape sequence
            i += 2;  // Skip ESC [
            std::string params;
            while (i < str.size() && str[i] != 'm') {
                params += str[i++];
            }
            i++;  // Skip 'm'

            // Parse SGR parameters
            if (params.empty() || params == "0") {
                // Reset all attributes
                current.fg_color = -1;
                current.bg_color = -1;
                current.fg_is_rgb = false;
                current.bg_is_rgb = false;
                current.bold = false;
                current.reset = true;
            } else {
                // Split parameters by semicolon
                std::vector<int> codes;
                size_t start = 0;
                size_t pos = 0;
                while (pos <= params.size()) {
                    if (pos == params.size() || params[pos] == ';') {
                        std::string param = params.substr(start, pos - start);
                        if (!param.empty()) {
                            try {
                                codes.push_back(std::stoi(param));
                            } catch (...) {
                                // Skip invalid parameter
                            }
                        }
                        start = pos + 1;
                    }
                    pos++;
                }

                // Process codes
                for (size_t j = 0; j < codes.size(); ++j) {
                    int code = codes[j];

                    if (code == 0) {
                        // Reset
                        current.fg_color = -1;
                        current.bg_color = -1;
                        current.fg_is_rgb = false;
                        current.bg_is_rgb = false;
                        current.bold = false;
                    } else if (code == 1) {
                        // Bold
                        current.bold = true;
                    } else if (code >= 30 && code <= 37) {
                        // Standard foreground colors (0-7)
                        current.fg_color = code - 30;
                        current.fg_is_rgb = false;
                    } else if (code == 38 && j + 1 < codes.size()) {
                        // Extended foreground color
                        if (codes[j + 1] == 5 && j + 2 < codes.size()) {
                            // 256-color mode: 38;5;N
                            current.fg_color = codes[j + 2];
                            current.fg_is_rgb = false;
                            j += 2;
                        } else if (codes[j + 1] == 2 && j + 4 < codes.size()) {
                            // RGB mode: 38;2;R;G;B
                            current.fg_rgb[0] = static_cast<uint8_t>(codes[j + 2]);
                            current.fg_rgb[1] = static_cast<uint8_t>(codes[j + 3]);
                            current.fg_rgb[2] = static_cast<uint8_t>(codes[j + 4]);
                            current.fg_is_rgb = true;
                            j += 4;
                        }
                    } else if (code >= 40 && code <= 47) {
                        // Standard background colors (0-7)
                        current.bg_color = code - 40;
                        current.bg_is_rgb = false;
                    } else if (code == 48 && j + 1 < codes.size()) {
                        // Extended background color
                        if (codes[j + 1] == 5 && j + 2 < codes.size()) {
                            // 256-color mode: 48;5;N
                            current.bg_color = codes[j + 2];
                            current.bg_is_rgb = false;
                            j += 2;
                        } else if (codes[j + 1] == 2 && j + 4 < codes.size()) {
                            // RGB mode: 48;2;R;G;B
                            current.bg_rgb[0] = static_cast<uint8_t>(codes[j + 2]);
                            current.bg_rgb[1] = static_cast<uint8_t>(codes[j + 3]);
                            current.bg_rgb[2] = static_cast<uint8_t>(codes[j + 4]);
                            current.bg_is_rgb = true;
                            j += 4;
                        }
                    } else if (code >= 90 && code <= 97) {
                        // Bright foreground colors (8-15)
                        current.fg_color = code - 90 + 8;
                        current.fg_is_rgb = false;
                    } else if (code >= 100 && code <= 107) {
                        // Bright background colors (8-15)
                        current.bg_color = code - 100 + 8;
                        current.bg_is_rgb = false;
                    }
                }
            }
        } else {
            // Regular character
            current.text += str[i++];
        }
    }

    // Add final segment
    if (!current.text.empty()) {
        segments.push_back(current);
    }

    return segments;
}

} // namespace fzf
