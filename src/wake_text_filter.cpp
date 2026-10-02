/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wake_text_filter.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace omni_agent {
namespace {

const std::vector<std::string>& separators() {
    static const std::vector<std::string> kSeparators = {
        " ", "\t", "\n", "\r",
        ".", ",", "!", "?", ":", ";", "-", "_",
        "。", "，", "！", "？", "：", "；", "、", "—",
    };
    return kSeparators;
}

size_t separatorLengthAt(const std::string& text, size_t offset) {
    for (const auto& separator : separators()) {
        if (text.compare(offset, separator.size(), separator) == 0) {
            return separator.size();
        }
    }
    return 0;
}

void trimLeadingSeparators(std::string* text) {
    bool changed = true;
    while (changed && !text->empty()) {
        changed = false;
        for (const auto& separator : separators()) {
            if (text->rfind(separator, 0) == 0) {
                text->erase(0, separator.size());
                changed = true;
                break;
            }
        }
    }
}

void trimTrailingAsciiWhitespace(std::string* text) {
    while (!text->empty() &&
            std::isspace(static_cast<unsigned char>(text->back()))) {
        text->pop_back();
    }
}

struct MatchText {
    std::string normalized;
    std::vector<size_t> original_end_offsets;
};

MatchText makeMatchText(const std::string& text) {
    MatchText result;
    size_t offset = 0;
    while (offset < text.size()) {
        const size_t separator_size = separatorLengthAt(text, offset);
        if (separator_size > 0) {
            offset += separator_size;
            continue;
        }

        unsigned char first = static_cast<unsigned char>(text[offset]);
        size_t codepoint_size = 1;
        if ((first & 0xE0U) == 0xC0U) {
            codepoint_size = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            codepoint_size = 3;
        } else if ((first & 0xF8U) == 0xF0U) {
            codepoint_size = 4;
        }
        codepoint_size = std::min(codepoint_size, text.size() - offset);

        for (size_t i = 0; i < codepoint_size; ++i) {
            unsigned char byte = static_cast<unsigned char>(text[offset + i]);
            if (codepoint_size == 1) {
                byte = static_cast<unsigned char>(std::tolower(byte));
            }
            result.normalized.push_back(static_cast<char>(byte));
            result.original_end_offsets.push_back(offset + codepoint_size);
        }
        offset += codepoint_size;
    }
    return result;
}

std::string normalizeForMatch(const std::string& text) {
    return makeMatchText(text).normalized;
}

}  // namespace

std::vector<WakePhraseConfig> defaultWakePhrases() {
    return {
        {
            "xiaojin",
            "小进小进",
            {
                "小金小金", "小静小静", "小晶小晶", "小鲸小鲸", "小井小井",
                "小新小新", "小鑫小鑫", "小近小近", "小劲小劲",
                "小丁小丁", "小姐小姐", "想金小金", "响金响金",
                "向金向金", "小心小心", "嗯", "Tentu", "Bien",
            },
        },
    };
}

std::vector<std::string> flattenWakePhrases(
        const std::vector<WakePhraseConfig>& phrases) {
    std::vector<std::string> flattened;
    std::set<std::string> seen;
    for (const auto& phrase : phrases) {
        const std::string canonical_key = normalizeForMatch(phrase.canonical);
        if (!canonical_key.empty() && seen.insert(canonical_key).second) {
            flattened.push_back(phrase.canonical);
        }
        for (const auto& alias : phrase.asr_aliases) {
            const std::string key = normalizeForMatch(alias);
            if (!key.empty() && seen.insert(key).second) {
                flattened.push_back(alias);
            }
        }
    }
    return flattened;
}

bool validateWakePhrases(
        const std::vector<WakePhraseConfig>& phrases,
        std::string* error) {
    if (phrases.empty()) {
        if (error) *error = "wake.phrases must not be empty";
        return false;
    }

    std::set<std::string> ids;
    std::set<std::string> texts;
    for (const auto& phrase : phrases) {
        if (phrase.id.empty()) {
            if (error) *error = "wake.phrases[].id must not be empty";
            return false;
        }
        if (!ids.insert(phrase.id).second) {
            if (error) *error = "duplicate wake phrase id: " + phrase.id;
            return false;
        }

        const std::string canonical = normalizeForMatch(phrase.canonical);
        if (canonical.empty()) {
            if (error) {
                *error = "wake phrase canonical must not be empty: " + phrase.id;
            }
            return false;
        }
        if (!texts.insert(canonical).second) {
            if (error) *error = "duplicate wake phrase: " + phrase.canonical;
            return false;
        }

        for (const auto& alias : phrase.asr_aliases) {
            const std::string normalized = normalizeForMatch(alias);
            if (normalized.empty()) {
                if (error) *error = "wake phrase alias must not be empty: " + phrase.id;
                return false;
            }
            if (!texts.insert(normalized).second) {
                if (error) *error = "duplicate wake phrase alias: " + alias;
                return false;
            }
        }
    }
    return true;
}

namespace {

// Longest wake phrase at the start of match_text; 0 when none matches.
size_t longestWakePrefix(
        const MatchText& match_text,
        const std::vector<std::string>& phrases,
        size_t* original_end,
        std::string* matched_phrase) {
    size_t best_normalized_size = 0;
    for (const auto& phrase : phrases) {
        const std::string normalized_phrase = normalizeForMatch(phrase);
        if (normalized_phrase.empty() ||
                normalized_phrase.size() > match_text.normalized.size()) {
            continue;
        }
        if (match_text.normalized.compare(
                0, normalized_phrase.size(), normalized_phrase) != 0) {
            continue;
        }
        if (normalized_phrase.size() > best_normalized_size) {
            best_normalized_size = normalized_phrase.size();
            *original_end =
                match_text.original_end_offsets[normalized_phrase.size() - 1];
            *matched_phrase = phrase;
        }
    }
    return best_normalized_size;
}

}  // namespace

WakeAsrTextFilterResult filterWakeAsrText(
        const std::string& text,
        const std::vector<std::string>& phrases) {
    std::string remaining = text;
    trimLeadingSeparators(&remaining);
    trimTrailingAsciiWhitespace(&remaining);

    // ASR often repeats the wake word ("小进小进小进小进，往前走"): strip every leading
    // occurrence, longest phrase first, so no wake word reaches the LLM as a command.
    std::string first_phrase;
    bool stripped = false;
    while (!remaining.empty()) {
        size_t original_end = 0;
        std::string phrase;
        if (longestWakePrefix(
                makeMatchText(remaining), phrases, &original_end, &phrase) == 0) {
            break;
        }
        if (!stripped) {
            first_phrase = phrase;
        }
        stripped = true;
        remaining.erase(0, original_end);
        trimLeadingSeparators(&remaining);
        trimTrailingAsciiWhitespace(&remaining);
    }

    if (!stripped) {
        return {text, "", false, false};
    }
    if (remaining.empty()) {
        return {"", first_phrase, true, true};
    }
    return {remaining, first_phrase, true, false};
}

bool shouldDropUnmatchedWakeAsr(
        bool contains_wake_event,
        int post_wake_speech_frames) {
    constexpr int kMinPostWakeSpeechFrames = 2;
    return contains_wake_event &&
        post_wake_speech_frames < kMinPostWakeSpeechFrames;
}

}  // namespace omni_agent
