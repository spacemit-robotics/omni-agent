/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * echo_null_replay - 离线复现 KWS 这一路的扬声器零陷（与 voice_chat_aec --kws-echo-null 同一份代码）。
 *
 * 输入 aec_dump_raw.wav（全部采集通道）和 aec_dump_ref.wav（对齐后的参考），输出零陷后的单声道
 * 以及同样延迟 22 ms 的参考；两者再送 AEC3（只开回声消除）即为线上 KWS 的输入。
 *
 * 用法:
 *   echo_null_replay --raw aec_dump_raw.wav --ref aec_dump_ref.wav --out null.wav --out-ref null_ref.wav
 *                    [--first-channel 2] [--log-every 10] [--no-gate] [--no-double-talk]
 */

#include <sndfile.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "echo_null_beamformer.hpp"

namespace {

bool writeMono(const std::string& path, const std::vector<float>& x) {
    SF_INFO info{};
    info.samplerate = EchoNullBeamformer::kSampleRate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) {
        std::cerr << "错误: 无法写 " << path << ": " << sf_strerror(nullptr) << "\n";
        return false;
    }
    sf_command(f, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    sf_writef_float(f, x.data(), static_cast<sf_count_t>(x.size()));
    sf_close(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string raw_path, ref_path, out_path, out_ref_path;
    int first_channel = 2;
    double log_every = 0.0;
    bool gate = true, double_talk = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--raw" && v) { raw_path = v; ++i; }
        else if (a == "--ref" && v) { ref_path = v; ++i; }
        else if (a == "--out" && v) { out_path = v; ++i; }
        else if (a == "--out-ref" && v) { out_ref_path = v; ++i; }
        else if (a == "--first-channel" && v) { first_channel = std::atoi(v); ++i; }
        else if (a == "--log-every" && v) { log_every = std::atof(v); ++i; }
        else if (a == "--no-gate") { gate = false; }
        else if (a == "--no-double-talk") { double_talk = false; }
        else {
            std::cerr << "用法: " << argv[0] << " --raw raw.wav --ref ref.wav --out null.wav --out-ref null_ref.wav"
                    " [--first-channel 2] [--log-every <s>] [--no-gate] [--no-double-talk]\n";
            return a == "-h" || a == "--help" ? 0 : 1;
        }
    }
    if (raw_path.empty() || ref_path.empty() || out_path.empty() || out_ref_path.empty()) {
        std::cerr << "错误: 需要 --raw --ref --out --out-ref\n";
        return 1;
    }

    SF_INFO raw_info{}, ref_info{};
    SNDFILE* raw_f = sf_open(raw_path.c_str(), SFM_READ, &raw_info);
    SNDFILE* ref_f = sf_open(ref_path.c_str(), SFM_READ, &ref_info);
    auto close_inputs = [&]() {
        if (raw_f) sf_close(raw_f);
        if (ref_f) sf_close(ref_f);
    };
    if (!raw_f || !ref_f) {
        std::cerr << "错误: 无法读取输入: " << sf_strerror(nullptr) << "\n";
        close_inputs();
        return 1;
    }
    if (raw_info.samplerate != EchoNullBeamformer::kSampleRate || ref_info.samplerate != raw_info.samplerate ||
            ref_info.channels != 1) {
        std::cerr << "错误: 需要 16 kHz 多声道 raw 和 16 kHz 单声道 ref\n";
        close_inputs();
        return 1;
    }
    const int channels = raw_info.channels;
    const sf_count_t frames = std::min(raw_info.frames, ref_info.frames);
    std::vector<float> raw(static_cast<size_t>(frames) * channels), ref(static_cast<size_t>(frames));
    const sf_count_t raw_read = sf_readf_float(raw_f, raw.data(), frames);
    const sf_count_t ref_read = sf_readf_float(ref_f, ref.data(), frames);
    close_inputs();
    if (raw_read != frames || ref_read != frames) {
        std::cerr << "错误: 输入读取不完整 (raw " << raw_read << "/" << frames << ", ref " << ref_read
            << "/" << frames << " 帧)\n";
        return 1;
    }

    EchoNullBeamformer null;
    EchoNullBeamformer::Options opt;
    opt.capture_channels = channels;
    opt.first_mic = first_channel - 1;
    opt.gate = gate;
    if (!double_talk) opt.double_talk_db = 0.0f;
    if (!null.Init(opt)) {
        std::cerr << "错误: ch" << first_channel << "-ch" << (first_channel + 2) << " 超出 " << channels << " 路输入\n";
        return 1;
    }

    const size_t hop = EchoNullBeamformer::kHop;
    const size_t n = static_cast<size_t>(frames) / hop * hop;
    std::vector<float> out(n), out_ref(n);
    double next_log = log_every;
    uint64_t relearns = 0;
    bool engaged = false;
    for (size_t pos = 0; pos < n; pos += hop) {
        null.Process(&raw[pos * channels], &ref[pos], hop, &out[pos], &out_ref[pos]);
        const auto& s = null.stats();
        const double t = static_cast<double>(pos + hop) / EchoNullBeamformer::kSampleRate;
        if (s.engaged != engaged || s.relearns != relearns) {
            std::cout << "t=" << t << "s " << (s.relearns != relearns ? "relearned" : "engaged")
                    << " after " << s.playback_s << " s of playback\n";
            engaged = s.engaged;
            relearns = s.relearns;
        }
        if (log_every > 0.0 && t >= next_log) {
            std::cout << "t=" << t << "s reduction=" << s.reduction_db << "dB playback=" << s.playback_s << "s\n";
            next_log += log_every;
        }
    }
    if (!writeMono(out_path, out) || !writeMono(out_ref_path, out_ref)) return 1;
    const auto& s = null.stats();
    std::cout << "done: " << n / static_cast<double>(EchoNullBeamformer::kSampleRate) << " s, engaged="
            << s.engaged << " last reduction=" << s.reduction_db << " dB relearns=" << s.relearns
            << " playback=" << s.playback_s << " s\n";
    return 0;
}
