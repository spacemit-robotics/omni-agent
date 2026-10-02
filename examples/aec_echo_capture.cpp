/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * aec_echo_capture - 通过与 voice_chat_aec 相同的 AEC 双工链路播放一组 WAV，
 * 同时写出逐帧对齐的 aec_dump_{raw,in,ref,out,echo_only}.wav（raw 为全部采集通道）。
 *
 * 用途：在某个机器人形态上采集“只有机器人自己在说话”的回声，用于测量 AEC
 * 和生成 KWS 双讲训练数据。播放期间附近不要有人说话。
 *
 * 用法:
 *   aec_echo_capture --play list.txt --dump-dir DIR [-i 0 -o 0]
 *   list.txt 每行一个 16 bit WAV（采样率须等于 --sample-rate），# 开头为注释。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "aec_duplex_processor.hpp"
#include "voice_common.hpp"

namespace {

std::atomic<bool> g_keep_playing{true};

void onSignal(int) {
    g_keep_playing = false;
}

struct Options {
    std::string play;
    std::string dump_dir;
    int input_device = -1;
    int output_device = -1;
    int sample_rate = 16000;
    int capture_channels = 4;
    int playback_channels = 2;
    int speech_channel = 2;
    double start_delay_ms = 150.0;   // same start as voice_chat_aec's warm-up
    double track_seconds = 15.0;     // delay tracker runs this long after stream open, then freezes
    int gap_ms = 800;
    float gain_db = 0.0f;
};

void usage(const char* argv0) {
    std::cout << "用法: " << argv0 << " --play <list.txt|file.wav> --dump-dir <dir> [选项]\n"
        << "  -i <id> / -o <id>          输入/输出设备 (默认: 系统默认)\n"
        << "  --sample-rate <hz>         AEC 采样率 (默认: 16000)\n"
        << "  --capture-channels <n>     采集通道数 (默认: 4)\n"
        << "  --playback-channels <n>    播放通道数 (默认: 2)\n"
        << "  --speech-channel <n>       送 AEC 的通道, 1 起 (默认: 2)\n"
        << "  --aec-delay <ms>           参考延迟初值 (默认: 150, 与 voice_chat_aec 预热一致)\n"
        << "  --track-seconds <s>        开流后延迟跟踪时长, 0 关闭 (默认: 15)\n"
        << "  --gap-ms <ms>              两个文件之间的静音 (默认: 800)\n"
        << "  --gain-db <db>             播放增益 (默认: 0)\n";
}

bool parseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else if (a == "--play" && (v = next())) o->play = v;
        else if (a == "--dump-dir" && (v = next())) o->dump_dir = v;
        else if (a == "-i" && (v = next())) o->input_device = std::atoi(v);
        else if (a == "-o" && (v = next())) o->output_device = std::atoi(v);
        else if (a == "--sample-rate" && (v = next())) o->sample_rate = std::atoi(v);
        else if (a == "--capture-channels" && (v = next())) o->capture_channels = std::atoi(v);
        else if (a == "--playback-channels" && (v = next())) o->playback_channels = std::atoi(v);
        else if (a == "--speech-channel" && (v = next())) o->speech_channel = std::atoi(v);
        else if (a == "--aec-delay" && (v = next())) o->start_delay_ms = std::atof(v);
        else if (a == "--track-seconds" && (v = next())) o->track_seconds = std::atof(v);
        else if (a == "--gap-ms" && (v = next())) o->gap_ms = std::atoi(v);
        else if (a == "--gain-db" && (v = next())) o->gain_db = static_cast<float>(std::atof(v));
        else { std::cerr << "错误: 未知或缺少参数的选项 " << a << "\n"; return false; }
    }
    if (o->play.empty() || o->dump_dir.empty()) {
        std::cerr << "错误: 必须指定 --play 和 --dump-dir\n";
        return false;
    }
    return true;
}

std::vector<std::string> readPlaylist(const std::string& path) {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".wav") == 0) return {path};
    std::vector<std::string> files;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') files.push_back(line);
    }
    return files;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage(argv[0]);
        return 1;
    }
    if (opt.sample_rate <= 0 || opt.gap_ms < 0 || opt.gap_ms > 60000) {
        std::cerr << "错误: --sample-rate 必须为正, --gap-ms 必须在 [0, 60000] 内\n";
        return 1;
    }
    const auto files = readPlaylist(opt.play);
    if (files.empty()) {
        std::cerr << "错误: 播放列表为空: " << opt.play << "\n";
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(opt.dump_dir, ec);
    if (ec) {
        std::cerr << "错误: 无法创建录音目录 " << opt.dump_dir << ": " << ec.message() << "\n";
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AecDuplexProcessor::Config cfg;
    cfg.sample_rate = opt.sample_rate;
    cfg.channels = 1;
    cfg.capture_channels = opt.capture_channels;
    cfg.playback_channels = opt.playback_channels;
    cfg.speech_channel = opt.speech_channel;
    cfg.frames_per_buffer = std::max(1, opt.sample_rate / 100);
    cfg.input_device = opt.input_device;
    cfg.output_device = opt.output_device;
    cfg.estimated_delay_ms = static_cast<int>(opt.start_delay_ms);
    cfg.dump_dir = opt.dump_dir;

    AecDuplexProcessor aec(cfg);
    if (!aec.initialize()) {
        std::cerr << "错误: AEC 初始化失败\n";
        return 1;
    }
    aec.setEchoOnlyAudioCallback([](const float*, size_t, int) {});  // writes the KWS-path track
    aec.setAudioCallback([](const float*, size_t, int) {});
    if (!aec.start()) {
        std::cerr << "错误: 无法打开音频流\n";
        return 1;
    }
    aec.setDelayMs(opt.start_delay_ms);
    if (opt.track_seconds > 0.0) {
        aec.setDelayTracking(true, 40.0, opt.track_seconds);
    }

    const float gain = std::pow(10.0f, opt.gain_db / 20.0f);
    const std::vector<float> gap(static_cast<size_t>(opt.sample_rate) * opt.gap_ms / 1000, 0.0f);
    double played_s = 0.0;
    size_t played = 0;
    for (size_t k = 0; k < files.size() && g_keep_playing; ++k) {
        AudioClip clip;
        std::string err;
        if (!loadWavMonoFloat(files[k], &clip, &err)) {
            std::cerr << "跳过 " << files[k] << ": " << err << "\n";
            continue;
        }
        if (clip.sample_rate != opt.sample_rate) {
            std::cerr << "跳过 " << files[k] << ": 采样率 " << clip.sample_rate << " != " << opt.sample_rate << "\n";
            continue;
        }
        for (float& s : clip.samples) s *= gain;
        aec.enqueuePlayback(clip.samples, opt.sample_rate);
        aec.enqueuePlayback(gap, opt.sample_rate);
        played_s += (clip.samples.size() + gap.size()) / static_cast<double>(opt.sample_rate);
        ++played;
        std::cout << getTimestamp() << " [" << (k + 1) << "/" << files.size() << "] " << files[k]
            << " (累计 " << static_cast<int>(played_s) << " s, 参考延迟 " << aec.delayMs() << " ms)\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (g_keep_playing && aec.isPlaying()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the last echo tail in
    aec.stop();
    std::cout << getTimestamp() << " 完成: 播放 " << played << " 个文件, " << static_cast<int>(played_s)
        << " s, 录音在 " << opt.dump_dir << "\n";
    return 0;
}
