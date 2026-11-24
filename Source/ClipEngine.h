#pragma once
// AutoClip Pro DSP building blocks: clip curves, filters, the safety limiter. No allocation after prepare().
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include "Features.h"

namespace ac
{
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)  { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// ── Clip curves ──────────────────────────────────────────────────────────────
// u = signal / threshold. Every curve has slope 1 at zero (quiet audio passes at the same level)
// and never goes past +-1 (the threshold).
inline float kneeClip (float a, float knee)   // a >= 0
{
    if (a <= knee) return a;
    const float r = 1.0f - knee;
    return knee + r * std::tanh ((a - knee) / r);
}

inline float shape (int type, float u)
{
    const float a = std::abs (u);
    switch (type)
    {
        case hard: return juce::jlimit (-1.0f, 1.0f, u);
        // Tape: smooth saturation over the whole range: softens everything a little, mostly 3rd harmonic.
        case tape: return std::tanh (u);
        // Tube: asymmetric. The positive half bends early and flattens 1.4 dB lower, the negative half bends
        // late and firmly: even (2nd) harmonics at every drive, like a single-ended triode.
        // The DC this makes is removed after the clipper.
        case tube: return u >= 0.0f ? 0.85f * kneeClip (a / 0.85f, 0.5f) : -kneeClip (a, 0.85f);
        // Soft: a smooth (analytic) knee: -0.02 dB at half the threshold, -1 dB at the threshold. No corner
        // anywhere, so the harmonics die away fast and there's very little left to alias.
        default:
        {
            const float u2 = u * u, u6 = u2 * u2 * u2;
            return u / std::pow (1.0f + u6, 1.0f / 6.0f);
        }
    }
}

// ── Filters ──────────────────────────────────────────────────────────────────
struct Biquad   // transposed direct form II, double precision (60 Hz at 384 kHz needs it)
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float process (float xf)
    {
        const double x = xf, y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
    void reset() { z1 = z2 = 0; }
    void setLowPass (double sr, double f, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        b0 = (1.0 - c) * 0.5 / a0; b1 = (1.0 - c) / a0; b2 = b0; a1 = -2.0 * c / a0; a2 = (1.0 - al) / a0;
    }
    void setHighPass (double sr, double f, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        b0 = (1.0 + c) * 0.5 / a0; b1 = -(1.0 + c) / a0; b2 = b0; a1 = -2.0 * c / a0; a2 = (1.0 - al) / a0;
    }
    void setHighShelf (double sr, double f, double gainDb, double q)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * f / sr;
        const double s = std::sin (w), c = std::cos (w), al = s / (2.0 * q), bt = 2.0 * std::sqrt (A) * al;
        const double a0 = (A + 1.0) - (A - 1.0) * c + bt;
        b0 = A * ((A + 1.0) + (A - 1.0) * c + bt) / a0; b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * c - bt) / a0; a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * c - bt) / a0;
    }
};

// BS.1770 K-weighting (loudness).
struct KWeight
{
    Biquad shelf, hp;
    void prepare (double sr) { shelf.setHighShelf (sr, 1681.974450955533, 4.0, 0.7071752369554196); hp.setHighPass (sr, 38.13547087602444, 0.5003270373253953); reset(); }
    void reset() { shelf.reset(); hp.reset(); }
    inline float process (float x) { return hp.process (shelf.process (x)); }
};

struct DcBlock
{
    float x1 = 0, y1 = 0, R = 0.9995f;
    void prepare (double sr) { R = (float) (1.0 - juce::MathConstants<double>::twoPi * 5.0 / sr); reset(); }
    void reset() { x1 = y1 = 0; }
    inline float process (float x) { const float y = x - x1 + R * y1; x1 = x; y1 = y; return y; }
};

// ── Safety limiter ───────────────────────────────────────────────────────────
// Stereo-linked lookahead peak limiter that makes sure nothing leaves the plugin above the ceiling.
// It catches what the clipper can't: the tiny overshoot from the oversampling filters, the transients
// MIX blends back in, and (TRUE PEAK on) the peaks between samples.
// Gain = sliding minimum of the needed gain over (attack + hold), then a moving average over the attack:
// the gain is always fully down in time for the peak (never late), ramps down over 1.5 ms, stays down
// 10 ms (so it doesn't flutter on every bass cycle) and then lets go over ~80 ms.
class Limiter
{
public:
    static constexpr int K = 16, phases = 7;   // inter-sample peak interpolator: 32 taps, 8x (7 in-between points)
    void prepare (double sr)
    {
        A  = juce::jmax (8, (int) std::lround (0.0015 * sr));
        Hd = A + juce::jmax (8, (int) std::lround (0.010 * sr));
        D  = A - 1 + K;
        for (auto& d : delay) d.assign ((size_t) D + 1, 0.0f);
        cDelay.assign ((size_t) D + 1, 1.0f);
        ma.assign ((size_t) A, 1.0f);
        dqIdx.assign ((size_t) Hd + 2, 0); dqVal.assign ((size_t) Hd + 2, 1.0f);
        relCoef = (float) (1.0 - std::exp (-1.0 / (0.08 * sr)));
        // 8x windowed-sinc interpolator, 32 taps per phase: estimates the waveform between samples.
        for (int ph = 0; ph < phases; ++ph)
        {
            const double frac = (ph + 1) / (double) (phases + 1);
            double sum = 0;
            for (int t = 0; t < 2 * K; ++t)
            {
                const double xx = (t - (K - 1)) - frac;   // taps at s-(K-1) .. s+K, centre at s + frac
                const double sinc = std::abs (xx) < 1e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * xx) / (juce::MathConstants<double>::pi * xx);
                const double win = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * xx / (double) K);   // Hann over +-K
                interp[ph][t] = (float) (sinc * win);
                sum += sinc * win;
            }
            for (int t = 0; t < 2 * K; ++t) interp[ph][t] = (float) (interp[ph][t] / sum);
        }
        reset (1.0f);
    }
    void reset (float ceiling)
    {
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (cDelay.begin(), cDelay.end(), ceiling);
        for (auto& h : hist) for (auto& v : h) v = 0.0f;
        for (auto& v : histC) v = ceiling;
        std::fill (ma.begin(), ma.end(), 1.0f);
        maSum = (double) A; g = 1.0f; pos = 0; maPos = 0; hPos = 0; recount = 0;
        n = 0; dqHead = dqTail = 0;
    }
    int latency() const { return D; }

    // Processes one stereo frame in place; `ceiling` belongs to this frame. Returns the gain applied.
    inline float process (float& l, float& r, float ceiling, bool truePeak)
    {
        hist[0][hPos] = l; hist[1][hPos] = r; histC[hPos] = ceiling;
        hPos = (hPos + 1) & (histSize - 1);
        // Peak at s = n - K (needs K future samples, which we have).
        auto H = [this] (int c, int back) { return hist[c][(hPos - 1 - back + 4 * histSize) & (histSize - 1)]; };
        float p = juce::jmax (std::abs (H (0, K)), std::abs (H (1, K)));
        if (truePeak)
            for (int c = 0; c < 2; ++c)
                for (int ph = 0; ph < phases; ++ph)
                {
                    float v = 0.0f;
                    for (int t = 0; t < 2 * K; ++t) v += interp[ph][t] * H (c, 2 * K - 1 - t);   // t=0 -> s-(K-1) ... t=2K-1 -> s+K = n
                    p = juce::jmax (p, std::abs (v));
                }
        // TRUE PEAK aims 0.25 dB under the ceiling: the estimate between samples is good, not perfect.
        const float cs = histC[(hPos - 1 - K + 4 * histSize) & (histSize - 1)] * (truePeak ? 0.9716f : 1.0f);
        const float need = p > cs ? cs / p : 1.0f;

        // sliding minimum over Hd samples (monotonic queue)
        const int cap = (int) dqVal.size();
        while (dqTail != dqHead && dqVal[(size_t) ((dqTail - 1 + cap) % cap)] >= need) dqTail = (dqTail - 1 + cap) % cap;
        dqIdx[(size_t) dqTail] = n; dqVal[(size_t) dqTail] = need; dqTail = (dqTail + 1) % cap;
        while (dqIdx[(size_t) dqHead] <= n - Hd) dqHead = (dqHead + 1) % cap;
        const float mn = dqVal[(size_t) dqHead];
        ++n;

        // moving average over the attack
        maSum += (double) mn - (double) ma[(size_t) maPos];
        ma[(size_t) maPos] = mn;
        if (++maPos >= A) maPos = 0;
        if (++recount >= 4096) { recount = 0; maSum = 0; for (float v : ma) maSum += v; }   // no drift
        const float G = (float) (maSum / (double) A);
        g = G < g ? G : g + (G - g) * relCoef;

        auto& dl = delay[0]; auto& dr = delay[1];
        dl[(size_t) pos] = l; dr[(size_t) pos] = r; cDelay[(size_t) pos] = ceiling;
        int rd = pos - D; if (rd < 0) rd += D + 1;
        const float co = cDelay[(size_t) rd];
        l = juce::jlimit (-co, co, dl[(size_t) rd] * g);
        r = juce::jlimit (-co, co, dr[(size_t) rd] * g);
        if (++pos > D) pos = 0;
        return g;
    }

private:
    static constexpr int histSize = 64;
    int A = 72, Hd = 552, D = 79, pos = 0, maPos = 0, hPos = 0, recount = 0, dqHead = 0, dqTail = 0;
    long long n = 0;
    std::vector<float> delay[2], cDelay, ma, dqVal;
    std::vector<long long> dqIdx;
    float hist[2][histSize] {}, histC[histSize] {};
    float interp[phases][2 * K] {};
    double maSum = 72.0;
    float g = 1.0f, relCoef = 0.001f;
};

// Plain delay line (sized in prepare).
struct Delay
{
    std::vector<float> buf; int n = 0, pos = 0;
    void prepare (int samples) { n = samples; buf.assign ((size_t) juce::jmax (1, samples), 0.0f); pos = 0; }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
    inline float process (float x)
    {
        if (n <= 0) return x;
        const float y = buf[(size_t) pos];
        buf[(size_t) pos] = x;
        if (++pos >= n) pos = 0;
        return y;
    }
};

// ── Bass finder (LOW PROTECT AUTO) ───────────────────────────────────────────
// Listens to the lows (decimated to ~1.5 kHz, 0.34 s FFT frames every 0.17 s) and finds, per frame, the
// lowest strong partial between 25 and 160 Hz: the kick / bass fundamental, not its harmonics. Frames that
// hold real low end vote; the crossover goes an octave above the 90th-percentile vote. No allocation
// after prepare(). Runs on the audio thread (one 512-point FFT per 0.17 s).
class BassFinder
{
public:
    static constexpr int order = 9, N = 1 << order, hop = N / 2, minVotes = 3;
    static constexpr float loHz = 25.0f, hiHz = 160.0f;   // anything over 75 Hz already maps to the top crossover
    void prepare (double sr)
    {
        D = juce::jmax (1, (int) std::lround (sr / 1500.0));
        fsD = sr / D;
        for (auto& f : lp) f.setLowPass (sr, 300.0, 0.7071067811865476);
        ring.assign ((size_t) N, 0.0f);
        fftData.assign ((size_t) N * 2, 0.0f);
        win.resize ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / N));
        fft = std::make_unique<juce::dsp::FFT> (order);
        reset();
    }
    void reset()   // forget everything heard (LEARN)
    {
        for (auto& f : lp) f.reset();
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (std::begin (votes), std::end (votes), 0);
        nVotes = 0; pkMax = 0.0f; dcount = 0; wpos = 0; sinceFrame = 0;
    }
    // Mono input at the plugin rate.
    inline void push (float x)
    {
        x = lp[1].process (lp[0].process (x));
        if (++dcount < D) return;
        dcount = 0;
        ring[(size_t) wpos] = x; wpos = (wpos + 1) & (N - 1);
        if (++sinceFrame >= hop) { sinceFrame = 0; frame(); }
    }
    int   votesSoFar() const { return nVotes; }
    // Crossover for what has been heard so far (0 = not enough low end heard yet).
    float crossoverHz() const
    {
        if (nVotes < minVotes) return 0.0f;
        const int want = (int) std::ceil (0.9 * nVotes);
        int acc = 0, hz = (int) loHz;
        for (int b = 0; b < numBins; ++b) { acc += votes[b]; if (acc >= want) { hz = (int) loHz + b; break; } }
        return juce::jlimit (autoMinHz, autoMaxHz, autoRatio * ((float) hz + 0.5f));
    }

private:
    void frame()
    {
        for (int i = 0; i < N; ++i) fftData[(size_t) i] = ring[(size_t) ((wpos + i) & (N - 1))] * win[(size_t) i];
        std::fill (fftData.begin() + N, fftData.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftData.data());
        const double bin = fsD / N;
        const int k0 = juce::jmax (2, (int) std::floor (loHz / bin)), k1 = juce::jmin (N / 2 - 2, (int) std::ceil (hiHz / bin));
        float fmax = 0.0f;
        for (int k = k0; k <= k1; ++k) fmax = juce::jmax (fmax, fftData[(size_t) k]);
        if (fmax < 1.0e-3f) return;                       // (512-point Hann: a -60 dBFS bass is ~0.1)
        pkMax = juce::jmax (pkMax, fmax);
        if (fmax < pkMax * 0.3f) return;                  // this frame has no real low end (vs the loudest heard)
        for (int k = k0; k <= k1; ++k)
        {
            const float m = fftData[(size_t) k];
            if (m >= fmax * 0.3f && m >= fftData[(size_t) k - 1] && m >= fftData[(size_t) k + 1])
            {
                // parabolic peak interpolation
                const float a = fftData[(size_t) k - 1], c = fftData[(size_t) k + 1], den = a - 2.0f * m + c;
                const float d = std::abs (den) > 1.0e-12f ? 0.5f * (a - c) / den : 0.0f;
                const double f = ((double) k + (double) juce::jlimit (-0.5f, 0.5f, d)) * bin;
                const int b = juce::jlimit (0, numBins - 1, (int) (f - loHz));
                ++votes[b]; ++nVotes;
                return;
            }
        }
    }
    static constexpr int numBins = (int) (hiHz - loHz) + 1;   // 1 Hz vote bins
    Biquad lp[2];
    int D = 32, dcount = 0, wpos = 0, sinceFrame = 0, nVotes = 0;
    double fsD = 1500.0;
    float pkMax = 0.0f;
    int votes[numBins] {};
    std::vector<float> ring, fftData, win;
    std::unique_ptr<juce::dsp::FFT> fft;
};
} // namespace ac
