/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_common.hpp"

#include <sndfile.h>

#include <memory>
#include <string>
#include <vector>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <ctime>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string homeDir() {
    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') {
            return home;
        }
    }
    return ".";
}

void trimWakeSeparators(std::string* text) {
    static const std::vector<std::string> kSeparators = {
        " ", "\t", "\n", "\r",
        ".", ",", "!", "?", ":", ";",
        "。", "，", "！", "？", "：", "；", "、",
    };

    bool changed = true;
    while (changed && !text->empty()) {
        changed = false;
        for (const auto& sep : kSeparators) {
            if (startsWith(*text, sep)) {
                text->erase(0, sep.size());
                changed = true;
                break;
            }
        }
    }

    changed = true;
    while (changed && !text->empty()) {
        changed = false;
        for (const auto& sep : kSeparators) {
            if (text->size() >= sep.size() &&
                    text->compare(text->size() - sep.size(), sep.size(), sep) == 0) {
                text->erase(text->size() - sep.size());
                changed = true;
                break;
            }
        }
    }
}

}  // namespace

// ============================================================================
// Global state
// ============================================================================

std::atomic<bool> g_running{true};
std::atomic<bool> g_processing{false};
std::atomic<bool> g_barge_in{false};
std::mutex g_process_thread_mutex;
std::unique_ptr<std::thread> g_process_thread;

void signalHandler(int sig) {
    (void)sig;
    std::cout << "\n" << getTimestamp() << " [退出中...]" << std::endl;
    g_running = false;
}

// ============================================================================
// Timestamp
// ============================================================================

std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << "[" << std::setfill('0')
        << std::setw(2) << tm_now.tm_hour << ":"
        << std::setw(2) << tm_now.tm_min << ":"
        << std::setw(2) << tm_now.tm_sec << "."
        << std::setw(3) << ms.count() << "]";
    return oss.str();
}

// ============================================================================
// TTS engine selection
// ============================================================================

const std::vector<std::pair<std::string, std::string>> kKokoroVoices = {
    // Chinese female
    {"zf_xiaobei",  "xiaobei"},
    {"zf_xiaoni",   "xiaoni"},
    {"zf_xiaoxiao", "xiaoxiao"},
    {"zf_xiaoyi",   "xiaoyi"},
    // Chinese male
    {"zm_yunxi",    "yunxi"},
    {"zm_yunyang",  "yunyang"},
    {"zm_yunjian",  "yunjian"},
    {"zm_yunfan",   "yunfan"},
    // American English female
    {"af_heart",    "heart"},
    {"af_alloy",    "alloy"},
    {"af_aoede",    "aoede"},
    {"af_bella",    "bella"},
    {"af_jessica",  "jessica"},
    {"af_kore",     "kore"},
    {"af_nicole",   "nicole"},
    {"af_nova",     "nova"},
    {"af_river",    "river"},
    {"af_sarah",    "sarah"},
    {"af_sky",      "sky"},
    // American English male
    {"am_adam",     "adam"},
    {"am_echo",     "echo"},
    {"am_eric",     "eric"},
    {"am_fenrir",   "fenrir"},
    {"am_liam",     "liam"},
    {"am_michael",  "michael"},
    {"am_onyx",     "onyx"},
    {"am_puck",     "puck"},
    // British English female
    {"bf_alice",    "alice"},
    {"bf_emma",     "emma"},
    {"bf_isabella", "isabella"},
    {"bf_lily",     "lily"},
    // British English male
    {"bm_daniel",   "daniel"},
    {"bm_fable",    "fable"},
    {"bm_george",   "george"},
    {"bm_lewis",    "lewis"},
};

std::string resolveVoiceName(const std::string& input) {
    if (input.empty()) return input;

    if (input.find('_') != std::string::npos) {
        return input;
    }

    std::vector<std::string> matches;
    for (const auto& [full, shortname] : kKokoroVoices) {
        if (shortname == input) {
            matches.push_back(full);
        }
    }

    if (matches.size() == 1) {
        std::cout << "音色: " << input << " -> " << matches[0] << std::endl;
        return matches[0];
    }

    if (matches.size() > 1) {
        std::cerr << "错误: 音色名 '" << input << "' 有多个匹配:\n";
        for (const auto& m : matches) {
            std::cerr << "  " << m << "\n";
        }
        std::cerr << "请使用完整名称，如 --tts kokoro:" << matches[0] << "\n";
        exit(1);
    }

    std::cerr << "警告: 未知音色 '" << input << "'，将直接使用该名称\n"
        << "使用 --list-voices 查看可用音色列表\n";
    return input;
}

void printVoiceList() {
    std::cout << "Kokoro 可用音色列表:\n"
        << "\n"
        << "中文女声 (zf_):\n"
        << "  zf_xiaobei      小北 (默认)\n"
        << "  zf_xiaoni       小妮\n"
        << "  zf_xiaoxiao     小小\n"
        << "  zf_xiaoyi       小一\n"
        << "\n"
        << "中文男声 (zm_):\n"
        << "  zm_yunxi        云希\n"
        << "  zm_yunyang      云阳\n"
        << "  zm_yunjian      云健\n"
        << "  zm_yunfan       云帆\n"
        << "\n"
        << "美式英语女声 (af_):\n"
        << "  af_heart        Heart\n"
        << "  af_alloy        Alloy\n"
        << "  af_aoede        Aoede\n"
        << "  af_bella        Bella\n"
        << "  af_jessica      Jessica\n"
        << "  af_kore         Kore\n"
        << "  af_nicole       Nicole\n"
        << "  af_nova         Nova\n"
        << "  af_river        River\n"
        << "  af_sarah        Sarah\n"
        << "  af_sky          Sky\n"
        << "\n"
        << "美式英语男声 (am_):\n"
        << "  am_adam         Adam\n"
        << "  am_echo         Echo\n"
        << "  am_eric         Eric\n"
        << "  am_fenrir       Fenrir\n"
        << "  am_liam         Liam\n"
        << "  am_michael      Michael\n"
        << "  am_onyx         Onyx\n"
        << "  am_puck         Puck\n"
        << "\n"
        << "英式英语女声 (bf_):\n"
        << "  bf_alice        Alice\n"
        << "  bf_emma         Emma\n"
        << "  bf_isabella     Isabella\n"
        << "  bf_lily         Lily\n"
        << "\n"
        << "英式英语男声 (bm_):\n"
        << "  bm_daniel       Daniel\n"
        << "  bm_fable        Fable\n"
        << "  bm_george       George\n"
        << "  bm_lewis        Lewis\n"
        << "\n"
        << "用法: --tts kokoro:<voice>  支持短名 (xiaobei) 和全名 (zf_xiaobei)\n"
        << std::endl;
}

EngineSelection parseEngine(const std::string& spec) {
    EngineSelection sel;
    sel.backend = SpacemiT::BackendType::MATCHA_ZH;

    auto colon = spec.find(':');
    std::string engine = (colon != std::string::npos) ? spec.substr(0, colon) : spec;
    std::string variant = (colon != std::string::npos) ? spec.substr(colon + 1) : "";

    if (engine == "matcha") {
        if (variant.empty() || variant == "zh") {
            sel.backend = SpacemiT::BackendType::MATCHA_ZH;
        } else if (variant == "en") {
            sel.backend = SpacemiT::BackendType::MATCHA_EN;
        } else if (variant == "zh-en" || variant == "zhen") {
            sel.backend = SpacemiT::BackendType::MATCHA_ZH_EN;
        } else {
            std::cerr << "错误: 未知 Matcha 变体 '" << variant << "'\n"
                << "可用变体: zh, en, zh-en\n";
            exit(1);
        }
        return sel;
    }

    if (engine == "kokoro") {
        sel.backend = SpacemiT::BackendType::KOKORO;
        sel.voice = resolveVoiceName(variant);
        return sel;
    }

    std::cerr << "错误: 未知引擎 '" << engine << "'\n"
        << "可用引擎: matcha, kokoro\n"
        << "用法: --tts matcha:zh 或 --tts kokoro:zf_xiaobei\n";
    exit(1);
}

// ============================================================================
// Audio conversion utilities
// ============================================================================

std::vector<float> pcm16BytesToFloat(const std::vector<uint8_t>& bytes) {
    size_t num_samples = bytes.size() / 2;
    std::vector<float> output(num_samples);
    const int16_t* samples = reinterpret_cast<const int16_t*>(bytes.data());

    for (size_t i = 0; i < num_samples; ++i) {
        output[i] = samples[i] / 32768.0f;
    }

    return output;
}

std::vector<int16_t> floatToPcm16(const std::vector<float>& samples) {
    std::vector<int16_t> pcm;
    pcm.reserve(samples.size());
    for (float sample : samples) {
        float clamped = std::clamp(sample, -1.0f, 1.0f);
        pcm.push_back(static_cast<int16_t>(clamped * 32767.0f));
    }
    return pcm;
}

WakeAsrTextFilterResult filterWakeAsrText(const std::string& text) {
    static const std::vector<std::string> kWakePrefixes = {
        "小进小进",
        "小金小金",
        "小静小静",
        "小晶小晶",
        "小鲸小鲸",
        "小新小新",
        "小鑫小鑫",
        "小近小近",
        "小劲小劲",
        "小丁小丁",
        "小姐小姐",
        "想金小金",
        "响金响金",
        "向金向金",
    };

    std::string filtered = text;
    trimWakeSeparators(&filtered);

    bool stripped = false;
    bool matched = true;
    while (matched && !filtered.empty()) {
        matched = false;
        for (const auto& prefix : kWakePrefixes) {
            if (startsWith(filtered, prefix)) {
                filtered.erase(0, prefix.size());
                trimWakeSeparators(&filtered);
                stripped = true;
                matched = true;
                break;
            }
        }
    }

    if (!stripped) {
        return {text, false, false};
    }
    if (filtered.empty()) {
        return {"", true, true};
    }
    return {filtered, true, false};
}

AsrAudioPreprocessStats preprocessAsrAudio(std::vector<float>* samples) {
    AsrAudioPreprocessStats stats;
    if (!samples || samples->empty()) {
        return stats;
    }

    double sum_sq = 0.0;
    float peak = 0.0f;
    for (float sample : *samples) {
        if (!std::isfinite(sample)) {
            continue;
        }
        float abs_sample = std::abs(sample);
        peak = std::max(peak, abs_sample);
        sum_sq += static_cast<double>(sample) * sample;
    }
    stats.input_peak = peak;
    stats.input_rms = static_cast<float>(std::sqrt(sum_sq / samples->size()));

    const float active_threshold = std::max(0.006f, peak * 0.05f);
    double active_sum_sq = 0.0;
    size_t active_count = 0;
    for (float sample : *samples) {
        if (!std::isfinite(sample)) {
            continue;
        }
        if (std::abs(sample) >= active_threshold) {
            active_sum_sq += static_cast<double>(sample) * sample;
            active_count++;
        }
    }
    const size_t min_active_count = std::max<size_t>(1, samples->size() / 100);
    if (active_count >= min_active_count) {
        stats.active_rms = static_cast<float>(
            std::sqrt(active_sum_sq / active_count));
    } else {
        stats.active_rms = stats.input_rms;
    }

    constexpr float kTargetRms = 0.09f;
    constexpr float kPeakLimit = 0.88f;
    constexpr float kMaxGain = 6.0f;

    float desired_gain = 1.0f;
    if (stats.active_rms > 0.0001f && stats.active_rms < kTargetRms) {
        desired_gain = kTargetRms / stats.active_rms;
    }
    float peak_gain = (peak > 0.0001f) ? (kPeakLimit / peak) : kMaxGain;
    stats.gain = std::min({desired_gain, peak_gain, kMaxGain});
    if (stats.gain < 0.01f) {
        stats.gain = 1.0f;
    }

    float output_peak = 0.0f;
    for (float& sample : *samples) {
        if (!std::isfinite(sample)) {
            sample = 0.0f;
        }
        sample *= stats.gain;
        if (sample > kPeakLimit) {
            sample = kPeakLimit;
            stats.clipped_samples++;
        } else if (sample < -kPeakLimit) {
            sample = -kPeakLimit;
            stats.clipped_samples++;
        }
        output_peak = std::max(output_peak, std::abs(sample));
    }
    stats.output_peak = output_peak;
    return stats;
}

std::string expandUserPath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
        return homeDir() + path.substr(1);
    }
    const std::string home_var = "$HOME";
    if (path.compare(0, home_var.size(), home_var) == 0 &&
            (path.size() == home_var.size() || path[home_var.size()] == '/')) {
        return homeDir() + path.substr(home_var.size());
    }
    return path;
}

bool loadWavMonoFloat(const std::string& filename, AudioClip* clip, std::string* error) {
    if (!clip) {
        if (error) *error = "output clip is null";
        return false;
    }

    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    SNDFILE* file = sf_open(filename.c_str(), SFM_READ, &info);
    if (!file) {
        if (error) *error = sf_strerror(nullptr);
        return false;
    }

    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        if (error) *error = "invalid wav format";
        return false;
    }

    std::vector<float> interleaved(static_cast<size_t>(info.frames) * info.channels);
    sf_count_t frames_read = sf_readf_float(file, interleaved.data(), info.frames);
    sf_close(file);
    if (frames_read <= 0) {
        if (error) *error = "empty wav data";
        return false;
    }

    clip->sample_rate = info.samplerate;
    clip->samples.assign(static_cast<size_t>(frames_read), 0.0f);
    for (sf_count_t frame = 0; frame < frames_read; ++frame) {
        float sum = 0.0f;
        for (int ch = 0; ch < info.channels; ++ch) {
            sum += interleaved[static_cast<size_t>(frame) * info.channels + ch];
        }
        clip->samples[static_cast<size_t>(frame)] = sum / info.channels;
    }
    return true;
}

void saveWav(const std::string& filename, const std::vector<int16_t>& data, int sample_rate) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "无法创建文件: " << filename << std::endl;
        return;
    }

    uint32_t data_size = static_cast<uint32_t>(data.size() * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;

    // RIFF header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&file_size), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;  // PCM
    uint16_t num_channels = 1;
    uint32_t sr = static_cast<uint32_t>(sample_rate);
    uint32_t byte_rate = sr * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    file.write(reinterpret_cast<const char*>(&fmt_size), 4);
    file.write(reinterpret_cast<const char*>(&audio_format), 2);
    file.write(reinterpret_cast<const char*>(&num_channels), 2);
    file.write(reinterpret_cast<const char*>(&sr), 4);
    file.write(reinterpret_cast<const char*>(&byte_rate), 4);
    file.write(reinterpret_cast<const char*>(&block_align), 2);
    file.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    // data chunk
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&data_size), 4);
    file.write(reinterpret_cast<const char*>(data.data()), data_size);
}
