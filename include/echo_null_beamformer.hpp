/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ECHO_NULL_BEAMFORMER_HPP
#define ECHO_NULL_BEAMFORMER_HPP

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Online loudspeaker null for the wake-word path.
 *
 * Per frequency bin, a 3-mic spatial filter that minimises the device's own playback while
 * keeping the first mic's gain (MVDR towards mic 1 over the echo covariance). The echo
 * covariance is learned from the frames in which the device is playing, so it needs no
 * calibration and no array/speaker geometry, and it re-learns a moved speaker. Talkers in any
 * direction other than the speaker's pass; no DOA is involved. Unlike a linear AEC it does not
 * have to predict the waveform, so loudspeaker distortion (same source position) is removed too.
 * The null is only applied while the device plays and for gate_hold_ms after it stops (long
 * enough to span the pauses between sentences); otherwise mic 1 passes through unchanged, so
 * wake-up in a quiet room behaves exactly as without it.
 *
 * 16 kHz only: 512-point STFT with a 10 ms hop. The output lags the input by kLatency samples;
 * Process() delays the reference by the same amount so a following AEC stays aligned.
 */
class EchoNullBeamformer {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kN = 512;
    static constexpr int kHop = 160;
    static constexpr int kMics = 3;
    static constexpr int kBins = kN / 2 + 1;
    static constexpr int kLatency = kN - kHop;

    struct Options {
        int capture_channels = 4;       // interleaved channels in the raw capture
        int first_mic = 1;              // 0-based index of the first of three consecutive mics
        float playback_dbfs = -45.0f;   // reference level that counts as playing
        int hangover_ms = 300;          // echo tail kept after the reference goes quiet
        double memory_s = 20.0;         // covariance forgetting time constant (playback time)
        double update_s = 0.5;          // weight update interval (playback time)
        double warmup_s = 3.0;          // pass mic 1 through until this much playback was seen
        float loading = 1e-3f;          // diagonal loading relative to trace / kMics
        // Relearn when the null stops removing the echo (speaker or array moved): the 25th
        // percentile of per-frame echo reduction over check_s of playback is above stale_db for
        // two windows in a row. The percentile ignores the frames where someone talks over the
        // playback, so double talk alone does not trigger it.
        float stale_db = -6.0f;
        double check_s = 2.0;
        bool gate = true;               // false: keep the null on between playbacks too
        int gate_hold_ms = 2000;        // keep the null on this long after playback stops
        int fade_ms = 40;               // null <-> pass-through crossfade at playback start/end
        // Once engaged, frames whose reduction is this much worse than the last window's median
        // hold a talker over the playback and are left out of the covariance: with three mics the
        // null has a second degree of freedom, and even a small share of the user's voice in the
        // covariance would be spent on nulling the user.
        float double_talk_db = 6.0f;    // <= 0 disables the exclusion
    };

    struct Stats {
        bool engaged = false;           // weights learned (false = mic 1 passed through)
        float reduction_db = 0.0f;      // median per-frame playback reduction, last check window
        uint64_t relearns = 0;
        uint64_t double_talk_frames = 0; // playback frames kept out of the covariance
        double playback_s = 0.0;        // playback time seen so far
    };

    EchoNullBeamformer() = default;

    // false if the options are out of range
    bool Init(const Options& options);

    // raw: frames x capture_channels interleaved; reference: frames, aligned with the echo.
    // frames must be a multiple of kHop. out / out_reference receive frames samples each,
    // both delayed by kLatency.
    void Process(const float* raw, const float* reference, size_t frames,
            float* out, float* out_reference);

    const Stats& stats() const { return stats_; }

private:
    using cd = std::complex<double>;
    using cf = std::complex<float>;

    void ProcessHop(const float* raw, const float* reference, float* out);
    void SolveWeights(const std::vector<cd>& cov, std::vector<cf>* weights) const;
    void CheckStale();

    Options opt_;
    Stats stats_;
    bool ready_ = false;

    // FFT (radix-2, in place)
    std::vector<int> rev_;
    std::vector<float> cos_, sin_, win_;
    void Fft(float* re, float* im) const;

    // STFT state
    std::vector<float> xbuf_;             // kMics x kN, newest samples at the end
    std::vector<float> ola_, nrm_;
    std::vector<float> re_, im_, yr_, yi_;
    std::vector<cf> x_;                   // kBins x kMics spectra of the current frame

    // Learning state: 3x3 covariances per bin, row-major
    std::vector<cd> cov_, block_, window_;
    std::vector<cf> w_cur_, w_target_;
    int hang_ = 0;
    int hangover_hops_ = 0;
    float gate_ = 0.0f;                   // 0 = mic 1 pass-through, 1 = null
    float gate_step_ = 1.0f;
    int gate_hold_hops_ = 0;
    int gate_hang_ = 0;
    int update_hops_ = 0;
    int check_hops_ = 0;
    int block_frames_ = 0;
    int window_frames_ = 0;
    uint64_t playback_frames_ = 0;
    double lambda_ = 0.0;
    int stale_windows_ = 0;
    bool have_reference_db_ = false;      // reduction_db measured while engaged
    std::vector<float> check_db_;

    std::vector<float> ref_delay_;        // last kLatency reference samples
};

#endif  // ECHO_NULL_BEAMFORMER_HPP
