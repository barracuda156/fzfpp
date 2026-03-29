#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <utf8.h>

namespace fzf {

// UTF-32 code point representation for efficient matching
using CodePoint = char32_t;

// ANSI color offset information
struct AnsiOffset {
    uint32_t offset;  // Byte offset in original string
    uint32_t color;   // Color code (simplified - can be extended)
};

// Represents a single input item
class Item {
public:
    Item() : index_(0) {}

    explicit Item(std::string text, size_t index = 0)
        : original_text_(std::move(text)), index_(index) {
        parse_text();
    }

    // Get original text
    const std::string& text() const { return original_text_; }

    // Get UTF-32 representation for matching
    const std::vector<CodePoint>& code_points() const { return code_points_; }

    // Get item index (original position in input)
    size_t index() const { return index_; }

    // Get display text (may differ from original if ANSI codes present)
    const std::string& display_text() const {
        return display_text_.empty() ? original_text_ : display_text_;
    }

    // Check if item has ANSI color codes
    bool has_ansi() const { return !ansi_offsets_.empty(); }

    // Get ANSI color offsets
    const std::vector<AnsiOffset>& ansi_offsets() const { return ansi_offsets_; }

    // Split text into fields and cache them
    void parse_fields(const std::string& delimiter) {
        if (delimiter.empty() || fields_parsed_) {
            return;
        }

        fields_.clear();
        size_t start = 0;
        size_t pos = original_text_.find(delimiter);

        while (pos != std::string::npos) {
            fields_.push_back(original_text_.substr(start, pos - start));
            start = pos + delimiter.length();
            pos = original_text_.find(delimiter, start);
        }
        fields_.push_back(original_text_.substr(start));
        fields_parsed_ = true;
    }

    // Get specific field (1-based indexing, like fzf)
    std::string get_field(int field_num) const {
        if (field_num < 1 || !fields_parsed_) {
            return "";
        }
        size_t idx = field_num - 1;
        if (idx >= fields_.size()) {
            return "";
        }
        return fields_[idx];
    }

    // Get display text based on field selection
    std::string get_display_fields(const std::vector<int>& field_nums) const {
        if (!fields_parsed_ || field_nums.empty()) {
            return original_text_;
        }

        std::string result;
        for (size_t i = 0; i < field_nums.size(); ++i) {
            if (i > 0) result += " ";
            result += get_field(field_nums[i]);
        }
        return result;
    }

    // Check if fields have been parsed
    bool has_fields() const { return fields_parsed_; }

private:
    void parse_text() {
        // Parse ANSI escape sequences and extract display text
        display_text_.clear();
        ansi_offsets_.clear();
        display_text_.reserve(original_text_.size());

        size_t i = 0;
        size_t display_pos = 0;
        bool in_escape = false;

        while (i < original_text_.size()) {
            unsigned char c = static_cast<unsigned char>(original_text_[i]);

            // Detect start of ANSI escape sequence (ESC [ or CSI)
            if (c == '\x1b' && i + 1 < original_text_.size() && original_text_[i + 1] == '[') {
                in_escape = true;
                i += 2;  // Skip ESC [
                continue;
            }

            if (in_escape) {
                // ANSI escape sequences end with a letter (A-Z, a-z)
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                    // Extract the color code (simplified - just store the whole sequence)
                    // Store offset at the display position where this code would apply
                    AnsiOffset offset;
                    offset.offset = static_cast<uint32_t>(display_pos);
                    // Store a simplified color representation (can be enhanced later)
                    offset.color = 0;  // Placeholder - could parse SGR parameters
                    ansi_offsets_.push_back(offset);

                    in_escape = false;
                }
                i++;
                continue;
            }

            // Normal character - add to display text
            display_text_ += original_text_[i];
            display_pos++;
            i++;
        }

        // If no ANSI codes were found, use original text as display text
        if (ansi_offsets_.empty()) {
            display_text_ = original_text_;
        }

        // Convert display text (without ANSI codes) to UTF-32 for efficient matching
        const std::string& text_for_matching = display_text_.empty() ? original_text_ : display_text_;

        try {
            utf8::utf8to32(text_for_matching.begin(), text_for_matching.end(),
                          std::back_inserter(code_points_));
        } catch (...) {
            // Invalid UTF-8, treat as Latin-1
            code_points_.clear();
            for (unsigned char c : text_for_matching) {
                code_points_.push_back(static_cast<CodePoint>(c));
            }
        }
    }

    std::string original_text_;
    std::string display_text_;  // Text without ANSI codes
    std::vector<CodePoint> code_points_;
    std::vector<AnsiOffset> ansi_offsets_;
    size_t index_;

    // Field support for --delimiter and --with-nth
    std::vector<std::string> fields_;
    bool fields_parsed_ = false;
};

// Match position information
struct MatchPos {
    uint32_t start;
    uint32_t end;
};

// Result of matching an item against a pattern
struct MatchResult {
    std::shared_ptr<Item> item;
    int32_t score;
    std::vector<MatchPos> positions;  // Matched character positions

    MatchResult() : score(0) {}

    MatchResult(std::shared_ptr<Item> i, int32_t s)
        : item(std::move(i)), score(s) {}

    MatchResult(std::shared_ptr<Item> i, int32_t s, std::vector<MatchPos> pos)
        : item(std::move(i)), score(s), positions(std::move(pos)) {}

    // Comparison for sorting (higher score is better)
    bool operator<(const MatchResult& other) const {
        if (score != other.score) {
            return score > other.score;  // Descending by score
        }
        // Tie-breaker: original index (stable sort)
        return item->index() < other.item->index();
    }
};

// Case sensitivity mode
enum class CaseMode {
    Smart,      // Case-insensitive if pattern is lowercase, otherwise sensitive
    Ignore,     // Always case-insensitive
    Respect     // Always case-sensitive
};

// Matching algorithm selection
enum class AlgoType {
    FuzzyV1,    // Fast greedy algorithm
    FuzzyV2     // Smith-Waterman optimal algorithm
};

} // namespace fzf
