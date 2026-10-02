/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "echo_null_beamformer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

bool EchoNullBeamformer::Init(const Options& options) {
    if (options.first_mic < 0 || options.first_mic + kMics > options.capture_channels ||
            options.memory_s <= 0.0 || options.update_s <= 0.0 || options.check_s <= 0.0) {
        return false;
    }
    opt_ = options;
    stats_ = Stats{};

    rev_.resize(kN);
    int bits = 0;
    while ((1 << bits) < kN) ++bits;
    for (int i = 0; i < kN; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        }
        rev_[i] = r;
    }
    cos_.resize(kN / 2);
    sin_.resize(kN / 2);
    for (int i = 0; i < kN / 2; ++i) {
        cos_[i] = static_cast<float>(std::cos(-2.0 * kPi * i / kN));
        sin_[i] = static_cast<float>(std::sin(-2.0 * kPi * i / kN));
    }
    win_.resize(kN);
    for (int i = 0; i < kN; ++i) {  // symmetric Hann, as in the offline calibration
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kN - 1)));
    }

    xbuf_.assign(static_cast<size_t>(kMics) * kN, 0.0f);
    ola_.assign(kN, 0.0f);
    nrm_.assign(kN, 0.0f);
    re_.resize(kN); im_.resize(kN); yr_.resize(kN); yi_.resize(kN);
    x_.assign(static_cast<size_t>(kBins) * kMics, cf{});

    const size_t cov_size = static_cast<size_t>(kBins) * kMics * kMics;
    cov_.assign(cov_size, cd{});
    block_.assign(cov_size, cd{});
    window_.assign(cov_size, cd{});
    w_cur_.assign(static_cast<size_t>(kBins) * kMics, cf{});
    for (int b = 0; b < kBins; ++b) w_cur_[static_cast<size_t>(b) * kMics] = 1.0f;
    w_target_ = w_cur_;

    const double hop_s = static_cast<double>(kHop) / kSampleRate;
    hangover_hops_ = std::max(0, static_cast<int>(std::lround(opt_.hangover_ms / 1000.0 / hop_s)));
    update_hops_ = std::max(1, static_cast<int>(std::lround(opt_.update_s / hop_s)));
    check_hops_ = std::max(1, static_cast<int>(std::lround(opt_.check_s / hop_s)));
    lambda_ = std::exp(-opt_.update_s / opt_.memory_s);
    gate_step_ = 1.0f / std::max(1.0f, static_cast<float>(opt_.fade_ms / 1000.0 / hop_s));
    gate_hold_hops_ = std::max(0, static_cast<int>(std::lround(opt_.gate_hold_ms / 1000.0 / hop_s)));
    gate_hang_ = 0;
    gate_ = 0.0f;
    hang_ = 0;
    block_frames_ = window_frames_ = 0;
    playback_frames_ = 0;
    stale_windows_ = 0;
    have_reference_db_ = false;
    check_db_.clear();
    ref_delay_.assign(kLatency, 0.0f);
    ready_ = true;
    return true;
}

void EchoNullBeamformer::Fft(float* re, float* im) const {
    for (int i = 0; i < kN; ++i) {
        if (i < rev_[i]) {
            std::swap(re[i], re[rev_[i]]);
            std::swap(im[i], im[rev_[i]]);
        }
    }
    for (int len = 2; len <= kN; len <<= 1) {
        const int half = len >> 1, step = kN / len;
        for (int i = 0; i < kN; i += len) {
            for (int j = 0; j < half; ++j) {
                const float wr = cos_[j * step], wi = sin_[j * step];
                const float xr = re[i + j + half], xi = im[i + j + half];
                const float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + j + half] = re[i + j] - tr;
                im[i + j + half] = im[i + j] - ti;
                re[i + j] += tr;
                im[i + j] += ti;
            }
        }
    }
}

void EchoNullBeamformer::Process(const float* raw, const float* reference, size_t frames,
        float* out, float* out_reference) {
    if (!ready_) {
        std::fill(out, out + frames, 0.0f);
        std::fill(out_reference, out_reference + frames, 0.0f);
        return;
    }
    const size_t stride = static_cast<size_t>(opt_.capture_channels);
    for (size_t pos = 0; pos + kHop <= frames; pos += kHop) {
        ProcessHop(raw + pos * stride, reference + pos, out + pos);
    }
    // reference delayed by kLatency: [ref_delay_ | reference] -> first frames samples
    for (size_t i = 0; i < frames; ++i) {
        out_reference[i] = i < ref_delay_.size() ? ref_delay_[i] : reference[i - ref_delay_.size()];
    }
    if (frames >= ref_delay_.size()) {
        std::copy(reference + frames - ref_delay_.size(), reference + frames, ref_delay_.begin());
    } else {
        std::rotate(ref_delay_.begin(), ref_delay_.begin() + static_cast<long>(frames), ref_delay_.end());
        std::copy(reference, reference + frames, ref_delay_.end() - static_cast<long>(frames));
    }
}

void EchoNullBeamformer::ProcessHop(const float* raw, const float* reference, float* out) {
    const size_t stride = static_cast<size_t>(opt_.capture_channels);

    // playback activity from the aligned reference, with a hangover for the echo tail
    double ref_power = 0.0;
    for (int i = 0; i < kHop; ++i) ref_power += static_cast<double>(reference[i]) * reference[i];
    ref_power /= kHop;
    if (10.0 * std::log10(ref_power + 1e-12) > opt_.playback_dbfs) {
        hang_ = hangover_hops_ + 1;
    } else if (hang_ > 0) {
        --hang_;
    }
    const bool playing = hang_ > 0;
    if (playing) {
        gate_hang_ = gate_hold_hops_ + 1;
    } else if (gate_hang_ > 0) {
        --gate_hang_;
    }

    // analysis: shift in the new hop and transform each mic
    const int keep = kN - kHop;
    for (int m = 0; m < kMics; ++m) {
        float* col = &xbuf_[static_cast<size_t>(m) * kN];
        std::memmove(col, col + kHop, sizeof(float) * keep);
        const size_t ch = static_cast<size_t>(opt_.first_mic + m);
        for (int i = 0; i < kHop; ++i) col[keep + i] = raw[i * stride + ch];
        for (int i = 0; i < kN; ++i) {
            re_[i] = col[i] * win_[i];
            im_[i] = 0.0f;
        }
        Fft(re_.data(), im_.data());
        for (int b = 0; b < kBins; ++b) x_[static_cast<size_t>(b) * kMics + m] = cf(re_[b], im_[b]);
    }

    // null only while playing: blend towards mic-1 pass-through (w = e1) otherwise
    const float target_gate = (stats_.engaged && (gate_hang_ > 0 || !opt_.gate)) ? 1.0f : 0.0f;
    gate_ = target_gate > gate_ ? std::min(target_gate, gate_ + gate_step_)
                                : std::max(target_gate, gate_ - gate_step_);

    // filter: y = w^H x with w = e1 + gate * (w_cur - e1); e_out uses the full null for the
    // staleness check
    double e_in = 0.0, e_out = 0.0;
    for (int b = 0; b < kBins; ++b) {
        const cf* x = &x_[static_cast<size_t>(b) * kMics];
        const cf* w = &w_cur_[static_cast<size_t>(b) * kMics];
        const cf y_null = std::conj(w[0]) * x[0] + std::conj(w[1]) * x[1] + std::conj(w[2]) * x[2];
        const cf y = gate_ >= 1.0f ? y_null : (gate_ <= 0.0f ? x[0] : x[0] + gate_ * (y_null - x[0]));
        yr_[b] = y.real();
        yi_[b] = y.imag();
        e_in += std::norm(x[0]);
        e_out += std::norm(y_null);
    }

    if (playing) {
        // per-frame reduction, skipping frames too quiet to say anything (about -60 dBFS)
        const bool loud = e_in > 1e-6 * kN * kN / 8.0;
        const float frame_db = static_cast<float>(10.0 * std::log10((e_out + 1e-20) / (e_in + 1e-20)));
        if (loud) check_db_.push_back(frame_db);
        // the relearn window takes every playback frame; the covariance skips double talk
        const bool double_talk = opt_.double_talk_db > 0.0f && stats_.engaged && loud && have_reference_db_ &&
            frame_db > stats_.reduction_db + opt_.double_talk_db;
        for (int b = 0; b < kBins; ++b) {
            const cf* x = &x_[static_cast<size_t>(b) * kMics];
            cd* blk = &block_[static_cast<size_t>(b) * kMics * kMics];
            cd* win = &window_[static_cast<size_t>(b) * kMics * kMics];
            for (int i = 0; i < kMics; ++i) {
                for (int j = 0; j < kMics; ++j) {
                    const cd v = cd(x[i]) * std::conj(cd(x[j]));
                    if (!double_talk) blk[i * kMics + j] += v;
                    win[i * kMics + j] += v;
                }
            }
        }
        if (!double_talk) ++block_frames_;
        ++window_frames_;
        ++playback_frames_;
        if (double_talk) ++stats_.double_talk_frames;
        stats_.playback_s = static_cast<double>(playback_frames_) * kHop / kSampleRate;

        if (block_frames_ >= update_hops_ && block_frames_ > 0) {
            const double a = 1.0 - lambda_;
            for (size_t k = 0; k < cov_.size(); ++k) {
                cov_[k] = lambda_ * cov_[k] + a * block_[k] / static_cast<double>(block_frames_);
                block_[k] = cd{};
            }
            block_frames_ = 0;
            if (stats_.playback_s >= opt_.warmup_s) {
                SolveWeights(cov_, &w_target_);
                stats_.engaged = true;
            }
        }
        if (window_frames_ >= check_hops_) CheckStale();
    }

    // glide towards the target weights (~100 ms) so a following AEC sees no jumps
    if (stats_.engaged) {
        for (size_t k = 0; k < w_cur_.size(); ++k) w_cur_[k] += 0.1f * (w_target_[k] - w_cur_[k]);
    }

    // synthesis: hermitian mirror, inverse FFT by conjugation, weighted overlap-add
    for (int b = kBins; b < kN; ++b) {
        yr_[b] = yr_[kN - b];
        yi_[b] = -yi_[kN - b];
    }
    for (int i = 0; i < kN; ++i) yi_[i] = -yi_[i];
    Fft(yr_.data(), yi_.data());
    const float inv = 1.0f / kN;
    std::memmove(ola_.data(), ola_.data() + kHop, sizeof(float) * keep);
    std::memmove(nrm_.data(), nrm_.data() + kHop, sizeof(float) * keep);
    std::fill(ola_.begin() + keep, ola_.end(), 0.0f);
    std::fill(nrm_.begin() + keep, nrm_.end(), 0.0f);
    for (int i = 0; i < kN; ++i) {
        ola_[i] += yr_[i] * inv * win_[i];
        nrm_[i] += win_[i] * win_[i];
    }
    for (int i = 0; i < kHop; ++i) out[i] = ola_[i] / std::max(nrm_[i], 1e-3f);
}

void EchoNullBeamformer::SolveWeights(const std::vector<cd>& cov, std::vector<cf>* weights) const {
    // w = R^-1 e1 / (e1^H R^-1 e1) = [1, C01 / C00, C02 / C00] with the cofactors of row 0 of
    // the loaded 3x3 Hermitian R.
    for (int b = 0; b < kBins; ++b) {
        const cd* r = &cov[static_cast<size_t>(b) * kMics * kMics];
        const double load = opt_.loading * (r[0].real() + r[4].real() + r[8].real()) / kMics + 1e-12;
        const double d = r[4].real() + load, f = r[8].real() + load;
        const cd b01 = r[1], c02 = r[2], e12 = r[5];
        const double c00 = d * f - std::norm(e12);
        const cd c01 = -(std::conj(b01) * f - e12 * std::conj(c02));
        const cd c02f = std::conj(b01) * std::conj(e12) - d * std::conj(c02);
        cf* w = &(*weights)[static_cast<size_t>(b) * kMics];
        if (!(c00 > 0.0) || !std::isfinite(c00)) {
            w[0] = 1.0f; w[1] = 0.0f; w[2] = 0.0f;
            continue;
        }
        w[0] = 1.0f;
        w[1] = cf(c01 / c00);
        w[2] = cf(c02f / c00);
    }
}

void EchoNullBeamformer::CheckStale() {
    if (!check_db_.empty()) {
        std::vector<float> v = check_db_;
        const size_t q = v.size() / 4, mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
        stats_.reduction_db = v[mid];
        have_reference_db_ = stats_.engaged;
        std::nth_element(v.begin(), v.begin() + static_cast<long>(q), v.end());
        const float p25 = v[q];
        stale_windows_ = (stats_.engaged && p25 > opt_.stale_db) ? stale_windows_ + 1 : 0;
    }
    if (stale_windows_ >= 2) {
        // start over from the last window only: the long-memory covariance describes the old path
        for (size_t k = 0; k < cov_.size(); ++k) cov_[k] = window_[k] / static_cast<double>(window_frames_);
        SolveWeights(cov_, &w_target_);
        w_cur_ = w_target_;
        ++stats_.relearns;
        stale_windows_ = 0;
    }
    for (auto& v : window_) v = cd{};
    window_frames_ = 0;
    check_db_.clear();
}
