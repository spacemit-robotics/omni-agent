/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WAKE_TEXT_FILTER_HPP
#define WAKE_TEXT_FILTER_HPP

#include <string>
#include <vector>

namespace omni_agent {

struct WakePhraseConfig {
    std::string id;
    std::string canonical;
    std::vector<std::string> asr_aliases;
};

struct WakeAsrTextFilterResult {
    std::string text;
    std::string matched_phrase;
    bool changed = false;
    bool drop = false;
};

std::vector<WakePhraseConfig> defaultWakePhrases();
std::vector<std::string> flattenWakePhrases(
    const std::vector<WakePhraseConfig>& phrases);
bool validateWakePhrases(
    const std::vector<WakePhraseConfig>& phrases,
    std::string* error);
WakeAsrTextFilterResult filterWakeAsrText(
    const std::string& text,
    const std::vector<std::string>& phrases);
bool shouldDropUnmatchedWakeAsr(
    bool contains_wake_event,
    int post_wake_speech_frames);

}  // namespace omni_agent

#endif  // WAKE_TEXT_FILTER_HPP
