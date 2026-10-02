// Synthetic checks for EchoNullBeamformer: pass-through before it has seen playback and again
// once playback stops, a deep null on a loudspeaker, relearning after the speaker moves, and no
// relearn from double talk alone.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "echo_null_beamformer.hpp"

namespace {

constexpr int kCh = 4;       // SPV layout: ch0 board output, ch1-ch3 raw mics
constexpr int kFirst = 1;
constexpr int kFs = EchoNullBeamformer::kSampleRate;

struct Path {                // per mic: direct + one reflection (integer delays)
    int d0[3], d1[3];
    float g0[3], g1[3];
};

const Path kSpeakerA = {{0, 4, 9}, {23, 31, 17}, {1.0f, 0.8f, 0.6f}, {0.3f, 0.25f, 0.35f}};
const Path kSpeakerB = {{9, 0, 4}, {17, 23, 31}, {0.6f, 1.0f, 0.8f}, {0.35f, 0.3f, 0.25f}};
const Path kTalker = {{6, 2, 0}, {40, 45, 38}, {0.9f, 1.0f, 0.95f}, {0.2f, 0.2f, 0.2f}};

int failures = 0;

void check(bool ok, const char* what, double value) {
    std::printf("%s %-58s %8.2f\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

float tap(const std::vector<float>& s, long t) { return t >= 0 && t < static_cast<long>(s.size()) ? s[t] : 0.0f; }

// mics[t * kCh + kFirst + m] += path applied to s
void add(std::vector<float>* mics, const std::vector<float>& s, const Path& p) {
    for (long t = 0; t < static_cast<long>(s.size()); ++t) {
        for (int m = 0; m < 3; ++m) {
            (*mics)[t * kCh + kFirst + m] += p.g0[m] * tap(s, t - p.d0[m]) + p.g1[m] * tap(s, t - p.d1[m]);
        }
    }
}

std::vector<float> noise(size_t n, float rms, std::mt19937* rng) {
    std::normal_distribution<float> g(0.0f, rms);
    std::vector<float> x(n);
    for (auto& v : x) v = g(*rng);
    return x;
}

struct Run {
    std::vector<float> out, mic1;
};

// Feeds mics/ref through the null in 10 ms hops; returns the output and mic 1 delayed like it.
Run feed(EchoNullBeamformer* null, const std::vector<float>& mics, const std::vector<float>& ref) {
    const size_t n = ref.size(), hop = EchoNullBeamformer::kHop;
    Run r;
    r.out.resize(n);
    r.mic1.resize(n);
    std::vector<float> out_ref(n);
    for (size_t pos = 0; pos + hop <= n; pos += hop) {
        null->Process(&mics[pos * kCh], &ref[pos], hop, &r.out[pos], &out_ref[pos]);
    }
    for (size_t t = 0; t < n; ++t) {
        const long src = static_cast<long>(t) - EchoNullBeamformer::kLatency;
        r.mic1[t] = src >= 0 ? mics[src * kCh + kFirst] : 0.0f;
    }
    return r;
}

double ratio_db(const Run& r, size_t from) {
    double a = 0.0, b = 0.0;
    for (size_t t = from; t < r.out.size(); ++t) {
        a += static_cast<double>(r.out[t]) * r.out[t];
        b += static_cast<double>(r.mic1[t]) * r.mic1[t];
    }
    return 10.0 * std::log10((a + 1e-20) / (b + 1e-20));
}

}  // namespace

int main() {
    std::mt19937 rng(20260925);
    EchoNullBeamformer null;
    EchoNullBeamformer::Options opt;
    opt.capture_channels = kCh;
    opt.first_mic = kFirst;
    if (!null.Init(opt)) {
        std::printf("FAIL Init\n");
        return 1;
    }
    {
        EchoNullBeamformer bad;
        EchoNullBeamformer::Options o = opt;
        o.first_mic = 2;  // ch2..ch4 of a 4-channel capture does not exist
        check(!bad.Init(o), "Init rejects mics outside the capture", 0.0);
    }

    // 1) no playback yet: mic 1 passes through, delayed by kLatency
    {
        const size_t n = kFs;
        std::vector<float> mics(n * kCh, 0.0f), ref(n, 0.0f);
        const auto talk = noise(n, 0.05f, &rng);
        add(&mics, talk, kTalker);
        Run r = feed(&null, mics, ref);
        double worst = 0.0;
        for (size_t t = EchoNullBeamformer::kN; t < n; ++t) worst = std::max(worst, std::fabs(double(r.out[t] - r.mic1[t])));
        check(worst < 1e-4, "pass-through before playback (max abs error)", worst);
        check(!null.stats().engaged, "not engaged without playback", 0.0);
    }

    // 2) 12 s of playback from speaker A: engaged, deep null
    std::vector<float> floor_noise;
    {
        const size_t n = 12 * kFs;
        const auto s = noise(n, 0.1f, &rng);
        std::vector<float> mics(n * kCh, 0.0f);
        add(&mics, s, kSpeakerA);
        floor_noise = noise(n * kCh, 1e-4f, &rng);
        for (size_t i = 0; i < mics.size(); ++i) mics[i] += floor_noise[i];
        Run r = feed(&null, mics, s);
        check(null.stats().engaged, "engaged after playback", null.stats().playback_s);
        const double red = ratio_db(r, n - 2 * kFs);
        check(red < -20.0, "echo reduction, last 2 s (dB)", red);
    }

    // 3) playback stops: back to exact mic-1 pass-through after hangover + hold + fade
    {
        const size_t n = 4 * kFs;
        const auto talk = noise(n, 0.05f, &rng);
        std::vector<float> mics(n * kCh, 0.0f), ref(n, 0.0f);
        add(&mics, talk, kTalker);
        Run r = feed(&null, mics, ref);
        double worst = 0.0;
        for (size_t t = 5 * kFs / 2 + EchoNullBeamformer::kN; t < n; ++t) {   // 0.3 s tail + 2 s hold + fade
            worst = std::max(worst, std::fabs(double(r.out[t] - r.mic1[t])));
        }
        check(worst < 1e-4, "pass-through once playback stopped (max abs error)", worst);
    }

    // 3b) a 1 s call from elsewhere in the middle of 4 s of playback from A: passes the null
    {
        const size_t n = 4 * kFs;
        auto talk = noise(n, 0.05f, &rng);
        for (size_t t = 0; t < n; ++t) if (t < 3 * kFs / 2 || t >= 5 * kFs / 2) talk[t] = 0.0f;
        std::vector<float> mics(n * kCh, 0.0f), ref(n, 0.0f), mics_talk(n * kCh, 0.0f);
        const auto s = noise(n, 0.1f, &rng);
        add(&mics, s, kSpeakerA);
        add(&mics, talk, kTalker);
        add(&mics_talk, talk, kTalker);
        EchoNullBeamformer probe = null;                    // same weights, no side effects
        Run both = feed(&probe, mics, s);
        // talker share of the output: output minus the output for echo alone
        EchoNullBeamformer probe2 = null;
        std::vector<float> mics_echo(n * kCh, 0.0f);
        add(&mics_echo, s, kSpeakerA);
        Run echo = feed(&probe2, mics_echo, s);
        double t_out = 0.0, t_in = 0.0;
        for (size_t t = 3 * kFs / 2 + EchoNullBeamformer::kLatency; t < 5 * kFs / 2 + EchoNullBeamformer::kLatency; ++t) {
            const double d = double(both.out[t]) - echo.out[t];
            t_out += d * d;
            const long src = static_cast<long>(t) - EchoNullBeamformer::kLatency;
            t_in += double(mics_talk[src * kCh + kFirst]) * mics_talk[src * kCh + kFirst];
        }
        const double keep = 10.0 * std::log10(t_out / t_in);
        check(keep > -3.0, "talker level through the null during playback (dB)", keep);
    }

    // 4) double talk: playback from A with a talker 10 dB below it half the time -> no relearn
    {
        const size_t n = 10 * kFs;
        const auto s = noise(n, 0.1f, &rng);
        auto talk = noise(n, 0.1f / std::sqrt(10.0f), &rng);
        for (size_t t = 0; t < n; ++t) if ((t / kFs) % 2) talk[t] = 0.0f;   // 1 s on, 1 s off
        std::vector<float> mics(n * kCh, 0.0f);
        add(&mics, s, kSpeakerA);
        add(&mics, talk, kTalker);
        const uint64_t before = null.stats().relearns;
        feed(&null, mics, s);
        check(null.stats().relearns == before, "no relearn from double talk", double(null.stats().relearns - before));
    }

    // 5) the speaker moves (A -> B): relearned within seconds, null deep again
    {
        const size_t n = 12 * kFs;
        const auto s = noise(n, 0.1f, &rng);
        std::vector<float> mics(n * kCh, 0.0f);
        add(&mics, s, kSpeakerB);
        const uint64_t before = null.stats().relearns;
        Run r = feed(&null, mics, s);
        check(null.stats().relearns > before, "relearned after the speaker moved", double(null.stats().relearns - before));
        const double red = ratio_db(r, n - 2 * kFs);
        check(red < -20.0, "echo reduction after the move, last 2 s (dB)", red);
    }

    std::printf("%s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
