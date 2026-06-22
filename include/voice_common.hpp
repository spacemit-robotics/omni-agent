/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOICE_COMMON_HPP
#define VOICE_COMMON_HPP

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <cstdint>

#include "tts_service.h"

// ============================================================================
// Global state
// ============================================================================

extern std::atomic<bool> g_running;
extern std::atomic<bool> g_processing;
extern std::atomic<bool> g_barge_in;
extern std::mutex g_process_thread_mutex;
extern std::unique_ptr<std::thread> g_process_thread;

void signalHandler(int sig);

// ============================================================================
// Timestamp
// ============================================================================

std::string getTimestamp();

// ============================================================================
// TTS engine selection
// ============================================================================

struct EngineSelection {
    SpacemiT::BackendType backend;
    std::string voice;
};

extern const std::vector<std::pair<std::string, std::string>> kKokoroVoices;

std::string resolveVoiceName(const std::string& input);
void printVoiceList();
EngineSelection parseEngine(const std::string& spec);

// ============================================================================
// Audio conversion utilities
// ============================================================================

struct AudioClip {
    std::vector<float> samples;
    int sample_rate = 0;
};

struct WakeAsrTextFilterResult {
    std::string text;
    bool changed = false;
    bool drop = false;
};

struct AsrAudioPreprocessStats {
    float input_rms = 0.0f;
    float active_rms = 0.0f;
    float input_peak = 0.0f;
    float gain = 1.0f;
    float output_peak = 0.0f;
    size_t clipped_samples = 0;
};

std::vector<float> pcm16BytesToFloat(const std::vector<uint8_t>& bytes);
std::vector<int16_t> floatToPcm16(const std::vector<float>& samples);
WakeAsrTextFilterResult filterWakeAsrText(const std::string& text);
AsrAudioPreprocessStats preprocessAsrAudio(std::vector<float>* samples);
bool loadWavMonoFloat(const std::string& filename, AudioClip* clip, std::string* error);
void saveWav(const std::string& filename, const std::vector<int16_t>& data, int sample_rate);

#endif  // VOICE_COMMON_HPP
