// AutoClip Pro offline test app.
// Usage:
//   AutoClipProTest <outdir> [quick] [name=key ...]      all tests (+ UI snapshots in <outdir>)
//   AutoClipProTest examples <outdir> <stems dir>         sound examples from real stems (loudness-matched)
//   AutoClipProTest renderfile <in.wav> <out.wav> [k=v]   one file through the plug-in (measurements, see renderFile)
//  - LOW PROTECT AUTO (crossover above the bass it hears), quiet-intro gain staging
//  - latency + alignment of every QUALITY setting
//  - output never above the ceiling (every type / quality / mix / protect / true peak)
//  - aliasing on a high sine, per type and QUALITY
//  - MATCH accuracy, loudness gain per AMOUNT
//  - LOW PROTECT: bass distortion with and without
//  - clicks when switching anything
//  - save -> new instance -> restore
//  - Lite can't use Pro features (bit-identical to plain Lite)
//  - licence keys, CPU, UI snapshots
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "LicenceTests.h"
#include <cmath>
#include <map>

using APP = AutoClipProAudioProcessor;
static double SR = 48000.0;
static constexpr int BS = 256;
static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

// ── Signals ──────────────────────────────────────────────────────────────────
using Buf = juce::AudioBuffer<float>;
static const double twoPi = juce::MathConstants<double>::twoPi;

static float kickAt (double t)
{
    if (t < 0 || t > 0.35) return 0.0f;
    const double f0 = 48.0, f1 = 170.0, k = 30.0;
    const double ph = twoPi * (f0 * t + (f1 - f0) * (1 - std::exp (-k * t)) / k);
    return (float) (std::sin (ph) * std::exp (-t / 0.11));
}

static Buf drumLoop (double secs, double bpm = 124.0, bool withKick = true)
{
    const int n = (int) (secs * SR);
    Buf b (2, n); b.clear();
    juce::Random rnd (42);
    const double step = 60.0 / bpm / 4.0;
    const int steps = (int) (secs / step) + 1;
    float prev = 0;
    for (int s = 0; s < steps; ++s)
    {
        const int pos16 = s % 16, bar = s / 16;
        const float barGain = (bar % 2 == 1) ? 0.6f : 1.0f;
        const float vel = barGain * (0.75f + 0.25f * rnd.nextFloat());
        const int start = (int) (s * step * SR);
        const bool kick = withKick && pos16 % 4 == 0, snare = pos16 == 4 || pos16 == 12, hat = pos16 % 2 != 0;
        for (int i = 0; i < (int) (0.4 * SR) && start + i < n; ++i)
        {
            const double t = i / SR;
            float l = 0, r = 0;
            if (kick) { const float k = 0.85f * vel * kickAt (t); l += k; r += k; }
            if (snare && t < 0.25)
            {
                const float nz = (rnd.nextFloat() * 2 - 1) * (float) std::exp (-t / 0.07) * 0.45f;
                const float tone = (float) (std::sin (twoPi * 190.0 * t) * std::exp (-t / 0.035)) * 0.35f;
                l += vel * (nz + tone); r += vel * (nz * 0.9f + tone);
            }
            if (hat && t < 0.08)
            {
                const float w = rnd.nextFloat() * 2 - 1; const float hp = w - prev; prev = w;
                const float h = hp * (float) std::exp (-t / 0.012) * 0.16f * vel;
                l += h * 0.7f; r += h;
            }
            b.addSample (0, start + i, l); b.addSample (1, start + i, r);
        }
    }
    b.applyGain (0.5f);
    return b;
}

static Buf bassPad (double secs, double bpm = 124.0)
{
    const int n = (int) (secs * SR);
    Buf b (2, n);
    const double bar = 4 * 60.0 / bpm;
    const double notes[] = { 55.0, 55.0, 73.42, 49.0 };
    const double chord[] = { 220.0, 261.63, 329.63, 392.0 };
    double bph = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / SR;
        const int barIdx = (int) (t / bar);
        const double inBar = std::fmod (t, bar), half = bar / 2;
        const double f = notes[barIdx % 4];
        bph += f / SR;
        double saw = 0; for (int k = 1; k <= 8; ++k) saw += std::sin (twoPi * k * bph) / k;
        const double noteT = std::fmod (inBar, half);
        const double env = std::min (1.0, noteT / 0.008) * (noteT > half - 0.03 ? (half - noteT) / 0.03 : 1.0);
        const double accent = (inBar < half ? 1.0 : 0.7);
        const float bass = (float) (0.32 * saw * env * accent);
        const double swell = 0.55 + 0.45 * std::sin (twoPi * t / 4.0);
        double pad = 0; for (int k = 0; k < 4; ++k) pad += std::sin (twoPi * chord[k] * t) + std::sin (twoPi * chord[k] * 1.004 * t + k);
        const float p = (float) (0.035 * pad * swell);
        b.setSample (0, i, bass + p); b.setSample (1, i, bass + p * 0.9f);
    }
    return b;
}

static Buf mixBus (double secs)
{
    auto d = drumLoop (secs), bp = bassPad (secs);
    for (int c = 0; c < 2; ++c) { d.applyGain (c, 0, d.getNumSamples(), 0.8f); d.addFrom (c, 0, bp, c, 0, d.getNumSamples(), 0.4f); }
    return d;
}

static Buf sine (double secs, double f, float amp)
{
    const int n = (int) (secs * SR);
    Buf b (2, n);
    for (int i = 0; i < n; ++i) { const float v = amp * (float) std::sin (twoPi * f * i / SR); b.setSample (0, i, v); b.setSample (1, i, v); }
    return b;
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static void setParam (APP& p, const char* id, float plain)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

static void setPro (APP& p, int kind)
{
    hz::LicenceInfo li; li.kind = (hz::LicenceInfo::Kind) kind; li.email = "buyer@example.com";
    li.expires = kind == hz::LicenceInfo::trial ? juce::Time::currentTimeMillis() / 1000 + 5 * 86400 - 60 : 0;
    p.setLicenceInfo (li);
}

static std::unique_ptr<APP> makeProc (bool pro, std::function<void (APP&)> setup = nullptr)
{
    auto p = std::make_unique<APP>();
    setPro (*p, pro ? hz::LicenceInfo::pro : hz::LicenceInfo::none);
    if (setup) setup (*p);
    p->setRateAndBufferSizeDetails (SR, BS);
    p->prepareToPlay (SR, BS);
    return p;
}

static Buf run (APP& p, const Buf& in, int len = -1, int offset = 0, int bs = BS)
{
    if (len < 0) len = in.getNumSamples();
    Buf out (2, len), blk (2, bs);
    juce::MidiBuffer m;
    for (int pos = 0; pos < len; pos += bs)
    {
        const int n = juce::jmin (bs, len - pos);
        blk.setSize (2, n, false, false, true);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) blk.setSample (c, i, in.getSample (c, (offset + pos + i) % in.getNumSamples()));
        p.processBlock (blk, m);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }
    return out;
}

static void writeWav (const juce::File& f, const Buf& b, double rate)
{
    f.deleteFile();
    juce::WavAudioFormat w;
    if (auto os = std::unique_ptr<juce::OutputStream> (f.createOutputStream().release()))
        if (auto wr = std::unique_ptr<juce::AudioFormatWriter> (w.createWriterFor (os.get(), rate, 2, 24, {}, 0)))
        { os.release(); wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
}

static float samplePeak (const Buf& b, int from = 0)
{
    float pk = 0; for (int c = 0; c < b.getNumChannels(); ++c) for (int i = from; i < b.getNumSamples(); ++i) pk = juce::jmax (pk, std::abs (b.getSample (c, i)));
    return pk;
}

// True peak, 8x windowed-sinc interpolation (64 taps), like a BS.1770 meter.
static float truePeak (const Buf& b, int from = 0)
{
    static float h[7][64]; static bool init = false;
    if (! init)
    {
        for (int ph = 1; ph < 8; ++ph)
        {
            double s = 0;
            for (int t = 0; t < 64; ++t) { const double x = (t - 31) - ph / 8.0; const double v = (std::abs (x) < 1e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x)) * (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x / 32.0)); h[ph - 1][t] = (float) v; s += v; }
            for (int t = 0; t < 64; ++t) h[ph - 1][t] = (float) (h[ph - 1][t] / s);
        }
        init = true;
    }
    float pk = samplePeak (b, from);
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* d = b.getReadPointer (c);
        for (int i = juce::jmax (from, 31); i < b.getNumSamples() - 33; ++i)
            for (int ph = 0; ph < 7; ++ph)
            {
                float v = 0; for (int t = 0; t < 64; ++t) v += h[ph][t] * d[i - 31 + t];
                pk = juce::jmax (pk, std::abs (v));
            }
    }
    return pk;
}

// BS.1770 integrated loudness (gated), LUFS.
static double lufs (const Buf& b, int from = 0, double rate = -1)
{
    if (rate < 0) rate = SR;
    ac::KWeight k[2]; for (auto& f : k) f.prepare (rate);
    const int n = b.getNumSamples() - from;
    std::vector<double> e ((size_t) juce::jmax (0, n));
    for (int i = 0; i < n; ++i)
    {
        double s = 0;
        for (int c = 0; c < 2; ++c) { const float y = k[c].process (b.getSample (juce::jmin (c, b.getNumChannels() - 1), from + i)); s += (double) y * y; }
        e[(size_t) i] = s;
    }
    const int blk = (int) (0.4 * rate), hop = (int) (0.1 * rate);
    std::vector<double> bl;
    for (int s = 0; s + blk <= n; s += hop) { double a = 0; for (int i = 0; i < blk; ++i) a += e[(size_t) (s + i)]; bl.push_back (a / blk); }
    auto L = [] (double p) { return -0.691 + 10 * std::log10 (p + 1e-20); };
    double sum = 0; int cnt = 0;
    for (double p : bl) if (L (p) > -70) { sum += p; ++cnt; }
    if (cnt == 0) return -120;
    const double rel = L (sum / cnt) - 10; sum = 0; cnt = 0;
    for (double p : bl) if (L (p) > rel) { sum += p; ++cnt; }
    return cnt ? L (sum / cnt) : -120;
}

static float maxDiff (const Buf& a, const Buf& b)
{
    float d = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i) d = juce::jmax (d, std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return d;
}

// ── Latency ──────────────────────────────────────────────────────────────────
static void latencyTests()
{
    std::printf ("\n== Latency and alignment ==\n");
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const double keep = SR; SR = rate;
        auto p = makeProc (true);
        const int L = p->getLatencySamples();
        std::printf ("  %6.0f Hz: reported latency %d samples (%.2f ms); chains OFF/2x/4x/8x = %d/%d/%d/%d + padding, limiter lookahead %d\n",
                     rate, L, 1000.0 * L / rate, p->getPathLatency (0), p->getPathLatency (1), p->getPathLatency (2), p->getPathLatency (3), L - p->getMaxPathLatency());
        // Alignment: HARD, AMOUNT 0 -> the clipper does nothing; the output must be the input (x staging gain) L samples later.
        juce::Random rnd (7);
        Buf noise (2, (int) (rate * 1.0));
        for (int i = 0; i < noise.getNumSamples(); ++i) { const float v = 0.25f * (rnd.nextFloat() * 2 - 1); noise.setSample (0, i, v); noise.setSample (1, i, v); }
        for (int os = 0; os < 4; ++os)
        {
            auto q = makeProc (true, [os] (APP& a) { setParam (a, "type", 1); setParam (a, "amount", 0); setParam (a, "quality", (float) os); setParam (a, "ceiling", 0); });
            check (q->getLatencySamples() == L, "latency the same for every QUALITY setting");
            auto out = run (*q, noise);
            int best = -1; double bestC = -1;
            for (int lag = 0; lag < L + 40; ++lag)
            {
                double c = 0; for (int i = (int) (0.3 * rate); i < noise.getNumSamples() - lag; ++i) c += noise.getSample (0, i) * out.getSample (0, i + lag);
                if (c > bestC) { bestC = c; best = lag; }
            }
            check (best == L, juce::String (rate, 0) + " Hz QUALITY " + juce::String (ac::osFactors[os]) + "x: output lines up with the reported latency (lag " + juce::String (best) + ")");
        }
        SR = keep;
    }
}

// ── Ceiling ──────────────────────────────────────────────────────────────────
static void ceilingTests()
{
    std::printf ("\n== Output never above the ceiling (AMOUNT 12, every type x QUALITY x MIX x LOW PROTECT x ceiling x TRUE PEAK) ==\n");
    const Buf sigs[2] = { drumLoop (2.5), mixBus (2.5) };
    int runs = 0, overs = 0; float worstOverDb = -100; float worstTpOff = -100, worstTpOn = -100, worstTpOn100 = -100;
    for (const auto& sig : sigs)
        for (int t = 0; t < ac::numTypes; ++t)
            for (int os = 0; os < 4; ++os)
                for (float mix : { 0.0f, 50.0f, 100.0f })
                    for (int prot : { 0, 2, (int) ac::protectAuto })
                        for (float ceil : { -0.3f, -6.0f })
                            for (int tp = 0; tp < 2; ++tp)
                            {
                                auto p = makeProc (true, [&] (APP& a) { setParam (a, "type", (float) t); setParam (a, "quality", (float) os); setParam (a, "mix", mix);
                                                                        setParam (a, "protect", (float) prot); setParam (a, "ceiling", ceil); setParam (a, "truepeak", (float) tp); setParam (a, "amount", 12); });
                                auto out = run (*p, sig);
                                const float c = juce::Decibels::decibelsToGain (ceil);
                                const float pk = samplePeak (out);
                                ++runs; if (pk > c * 1.000001f) ++overs;
                                worstOverDb = juce::jmax (worstOverDb, juce::Decibels::gainToDecibels (pk / c));
                                if (sig.getNumSamples() > 0 && os == 2 && t < 2 && prot == 0)
                                {
                                    const float tpk = juce::Decibels::gainToDecibels (truePeak (out) / c);
                                    if (tp) worstTpOn = juce::jmax (worstTpOn, tpk); else worstTpOff = juce::jmax (worstTpOff, tpk);
                                    if (tp && mix > 99.0f) worstTpOn100 = juce::jmax (worstTpOn100, tpk);
                                    if (std::getenv ("TPDBG")) std::printf ("    tp=%d type=%d mix=%.0f ceil=%.1f sig=%d -> %+.2f dB\n", tp, t, mix, ceil, (int) (&sig - sigs), tpk);
                                }
                            }
    std::printf ("  %d renders, highest sample peak vs ceiling: %+.4f dB\n", runs, worstOverDb);
    check (overs == 0, "no sample above the ceiling in " + juce::String (runs) + " renders");
    std::printf ("  true peak vs ceiling (SOFT/HARD, 4x, 8x-interpolated meter): TRUE PEAK off %+.2f dB, on %+.2f dB (MIX 100: %+.2f dB)\n", worstTpOff, worstTpOn, worstTpOn100);
    check (worstTpOn <= 0.2f, "TRUE PEAK on keeps inter-sample peaks within 0.2 dB of the ceiling, worst case (" + juce::String (worstTpOn, 2) + " dB)");

    // Sample rates + odd block sizes (host stress)
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        const double keep = SR; SR = rate;
        auto sig = mixBus (1.5);
        for (int bs : { 1, 17, 1024, 4096 })
        {
            auto p = makeProc (true, [] (APP& a) { setParam (a, "type", 1); setParam (a, "amount", 12); setParam (a, "quality", 3); setParam (a, "mix", 50); });
            auto out = run (*p, sig, -1, 0, bs);
            check (samplePeak (out) <= juce::Decibels::decibelsToGain (-0.3f) * 1.000001f, juce::String (rate, 0) + " Hz, block " + juce::String (bs) + ": under the ceiling");
        }
        SR = keep;
    }
}

// ── Aliasing ─────────────────────────────────────────────────────────────────
static double aliasDbc (const Buf& out, double f)
{
    const int order = 15, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> d ((size_t) N * 2, 0.0f);
    const int st = out.getNumSamples() - N;
    for (int i = 0; i < N; ++i)
    {
        const double w = 0.35875 - 0.48829 * std::cos (twoPi * i / N) + 0.14128 * std::cos (2 * twoPi * i / N) - 0.01168 * std::cos (3 * twoPi * i / N);
        d[(size_t) i] = (float) (out.getSample (0, st + i) * w);
    }
    fft.performFrequencyOnlyForwardTransform (d.data());
    const double bin = SR / N;
    double fund = 0, alias = 0;
    for (int k = 3; k < N / 2; ++k)
    {
        const double fk = k * bin, pw = (double) d[(size_t) k] * d[(size_t) k];
        bool harmonic = false;
        for (int h = 1; h * f < SR / 2; ++h) if (std::abs (fk - h * f) <= 6 * bin) { harmonic = true; if (h == 1) fund += pw; }
        if (! harmonic) alias += pw;
    }
    return 10 * std::log10 (alias / fund + 1e-30);
}

static void aliasTests()
{
    std::printf ("\n== Aliasing: 7 kHz sine at 48 kHz, AMOUNT 6 dB into the clipper (energy that isn't a real harmonic, dBc) ==\n");
    const double f = std::round (7000.0 / (SR / 32768)) * (SR / 32768);
    const auto sig = sine (1.5, f, 0.5f);
    std::printf ("  %-6s %8s %8s %8s %8s\n", "type", "OFF", "2X", "4X", "8X");
    double res[ac::numTypes][4];
    for (int t = 0; t < ac::numTypes; ++t)
    {
        for (int os = 0; os < 4; ++os)
        {
            auto p = makeProc (true, [&] (APP& a) { setParam (a, "type", (float) t); setParam (a, "quality", (float) os); setParam (a, "amount", 6); });
            res[t][os] = aliasDbc (run (*p, sig), f);
        }
        std::printf ("  %-6s %8.1f %8.1f %8.1f %8.1f\n", ac::typeName (t), res[t][0], res[t][1], res[t][2], res[t][3]);
        check (res[t][1] < res[t][0] && res[t][2] < res[t][1] && res[t][3] < res[t][2] + 1.0, juce::String (ac::typeName (t)) + ": aliasing goes down with oversampling, never up");
        check (res[t][3] <= res[t][0] - 20.0, juce::String (ac::typeName (t)) + ": 8x is at least 20 dB cleaner than OFF (" + juce::String (res[t][0] - res[t][3], 1) + " dB)");
    }
    check (res[ac::soft][ac::gate::liteOsIndex] < -70.0, "Lite (SOFT, 4x) aliasing below -70 dBc (" + juce::String (res[ac::soft][ac::gate::liteOsIndex], 1) + ")");
}

// ── Loudness + MATCH ─────────────────────────────────────────────────────────
static void loudnessTests()
{
    std::printf ("\n== Loudness (Lite, 12 s, first 3 s skipped; 'vs norm' = vs the dry track peak-normalised to the same ceiling) ==\n");
    struct S { const char* name; Buf sig; };
    std::vector<S> sigs = { { "drums", drumLoop (12) }, { "bass/pad", bassPad (12) }, { "mix bus", mixBus (12) } };
    const int from = (int) (3 * SR);
    for (auto& s : sigs)
    {
        const double inL = lufs (s.sig, from);
        const double normL = inL + (-0.3 - juce::Decibels::gainToDecibels (samplePeak (s.sig)));
        std::printf ("  %-8s dry %.1f LUFS, peak-normalised %.1f LUFS\n", s.name, inL, normL);
        for (float amt : { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f })
        {
            auto p = makeProc (false, [amt] (APP& a) { setParam (a, "amount", amt); });
            auto out = run (*p, s.sig);
            const double o = lufs (out, from);
            auto pm = makeProc (false, [amt] (APP& a) { setParam (a, "amount", amt); setParam (a, "match", 1); });
            const double om = lufs (run (*pm, s.sig), from);
            std::printf ("     AMOUNT %4.1f: %6.1f LUFS (%+5.1f vs dry, %+5.1f vs norm), peak %5.2f dB | MATCH on: %+5.2f LU vs dry\n",
                         amt, o, o - inL, o - normL, juce::Decibels::gainToDecibels (samplePeak (out, from)), om - inL);
            check (std::abs (om - inL) <= 0.5, juce::String ("MATCH within 0.5 LU: ") + s.name + " AMOUNT " + juce::String (amt, 0) + " (" + juce::String (om - inL, 2) + ")");
            if (amt >= 6.0f) check (o - normL > 1.5, juce::String ("AMOUNT ") + juce::String (amt, 0) + " louder than plain normalising: " + s.name);
        }
    }
}

// ── LOW PROTECT ──────────────────────────────────────────────────────────────
static double bassHarmonicsDb (const Buf& out, double f0)
{
    const int order = 16, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> d ((size_t) N * 2, 0.0f);
    const int st = out.getNumSamples() - N;
    for (int i = 0; i < N; ++i) d[(size_t) i] = (float) (out.getSample (0, st + i) * (0.5 - 0.5 * std::cos (twoPi * i / N)));
    fft.performFrequencyOnlyForwardTransform (d.data());
    const double bin = SR / N;
    auto band = [&] (double f) { double s = 0; for (int k = (int) (f / bin) - 3; k <= (int) (f / bin) + 3; ++k) s += (double) d[(size_t) k] * d[(size_t) k]; return s; };
    double h = 0; for (int k = 2; k <= 7; ++k) h += band (k * f0);
    return 10 * std::log10 (h / band (f0));
}

static void protectTests()
{
    std::printf ("\n== LOW PROTECT: 55 Hz bass + hats/snare (bass alone stays under the ceiling, the drums push the peaks over),\n"
                 "   AMOUNT 6; bass harmonics (110-385 Hz) vs the fundamental ==\n");
    const double f0 = std::round (55.0 / (SR / 65536)) * (SR / 65536);
    auto sig = sine (3.0, f0, 0.3f);
    auto drums = drumLoop (3.0, 124.0, false);
    // high-pass the drums so the test band (110-385 Hz) only holds bass harmonics
    for (int c = 0; c < 2; ++c) { ac::Biquad hp1, hp2; hp1.setHighPass (SR, 1500, 0.707); hp2.setHighPass (SR, 1500, 0.707);
        for (int i = 0; i < sig.getNumSamples(); ++i) sig.addSample (c, i, 1.6f * hp2.process (hp1.process (drums.getSample (c, i)))); }
    std::printf ("  dry input: %.1f dB; bass is %.1f dB of the peak\n", bassHarmonicsDb (sig, f0), juce::Decibels::gainToDecibels (0.3f / samplePeak (sig)));
    for (int t = 0; t < ac::numTypes; ++t)
    {
        double r[ac::numProtectSteps];
        for (int prot = 0; prot < ac::numProtectSteps; ++prot)
        {
            auto p = makeProc (true, [&] (APP& a) { setParam (a, "type", (float) t); setParam (a, "amount", 6); setParam (a, "protect", (float) prot); });
            r[prot] = bassHarmonicsDb (run (*p, sig), f0);
        }
        std::printf ("  %-5s OFF %6.1f | 60 Hz %6.1f | 90 Hz %6.1f | 120 Hz %6.1f | 150 Hz %6.1f dB\n", ac::typeName (t), r[0], r[1], r[2], r[3], r[4]);
        // HARD and the (tight-kneed) SOFT curve only touch the spikes, so the bass is clean with or without
        // protection; TAPE and TUBE bend the whole wave and need it.
        if (t == ac::hard || t == ac::soft)
            check (r[3] <= -40.0 && r[3] <= r[0] + 3.5, juce::String (ac::typeName (t)) + ": bass harmonics stay under -40 dB with LOW PROTECT 120 Hz (" + juce::String (r[3], 1) + " dB, OFF " + juce::String (r[0], 1) + ")");
        else check (r[3] < r[0] - 6.0, juce::String (ac::typeName (t)) + ": LOW PROTECT 120 Hz cuts bass distortion by > 6 dB (" + juce::String (r[0] - r[3], 1) + " dB)");
    }
}

// ── LOW PROTECT AUTO ─────────────────────────────────────────────────────────
static Buf bassLine (double secs, std::vector<double> notes, double noteSecs, float amp = 0.3f)
{
    const int n = (int) (secs * SR);
    Buf b (2, n); double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / SR, f = notes[(size_t) ((int) (t / noteSecs) % (int) notes.size())];
        ph += f / SR;
        double saw = 0; for (int k = 1; k <= 8; ++k) saw += std::sin (twoPi * k * ph) / k;
        const float v = amp * 0.6f * (float) saw;
        b.setSample (0, i, v); b.setSample (1, i, v);
    }
    return b;
}

static void protectAutoTests()
{
    std::printf ("\n== LOW PROTECT AUTO: the crossover goes an octave above the lowest strong bass note it hears ==\n");
    auto withDrums = [] (Buf b)
    {
        auto d = drumLoop (b.getNumSamples() / SR, 124.0, true);
        for (int c = 0; c < 2; ++c) b.addFrom (c, 0, d, c, 0, b.getNumSamples(), 0.8f);
        return b;
    };
    auto check2 = [] (APP& p) { return p.apvts.getRawParameterValue ("protect")->load(); };
    {
        auto p = makeProc (true);
        check ((int) check2 (*p) == ac::protectAuto, "Pro default is LOW PROTECT AUTO");
    }
    struct Case { const char* name; Buf sig; double fLow, fTop; };
    std::vector<Case> cases = {
        { "55 Hz sine bass",                     sine (6.0, 55.0, 0.4f), 55.0, 55.0 },
        { "55 Hz saw bass + drums",              withDrums (bassLine (6.0, { 55.0 }, 1.0)), 55.0, 55.0 },
        { "41 Hz (E1) saw bass + drums",         withDrums (bassLine (6.0, { 41.2 }, 1.0)), 41.2, 41.2 },
        { "bassline E1-A1-D2 + drums",           withDrums (bassLine (8.0, { 41.2, 55.0, 73.4, 55.0 }, 0.5)), 41.2, 73.4 },
        { "kick-only drum loop (48-170 Hz kick)", drumLoop (6.0), 48.0, 48.0 },
        { "mix bus (bass 49-73 Hz, pad, drums)", mixBus (8.0), 49.0, 73.4 },
    };
    auto xo = [] (double f) { return juce::jlimit (60.0, 150.0, 2.0 * f); };
    for (auto& c : cases)
    {
        auto p = makeProc (true, [] (APP& a) { setParam (a, "amount", 6); });
        float lowest = 1000.0f;
        const int len = c.sig.getNumSamples();
        Buf blk (2, BS); juce::MidiBuffer m;
        for (int pos = 0; pos + BS <= len; pos += BS)
        {
            for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, c.sig, ch, pos, BS);
            p->processBlock (blk, m);
            if (pos > (int) (2.0 * SR)) lowest = juce::jmin (lowest, p->getAutoProtectHz());
        }
        const float hz = p->getAutoProtectHz();
        std::printf ("  %-38s -> AUTO %5.1f Hz (lowest after 2 s %5.1f; octave over the lowest / highest bass note %.0f / %.0f Hz)\n", c.name, hz, lowest, xo (c.fLow), xo (c.fTop));
        check (lowest >= 0.92 * xo (c.fLow) && hz >= 0.92 * xo (c.fTop) && hz <= 150.0f,
               juce::String ("AUTO on ") + c.name + ": " + juce::String (hz, 1) + " Hz, never under an octave over the bass");
    }
    // The old trap: 60 Hz on a 55 Hz bass. AUTO must protect the bass at least as well as the 120 Hz step.
    {
        const double f0 = std::round (55.0 / (SR / 65536)) * (SR / 65536);
        auto sig = sine (4.0, f0, 0.3f);
        auto drums = drumLoop (4.0, 124.0, false);
        for (int c = 0; c < 2; ++c) { ac::Biquad hp1, hp2; hp1.setHighPass (SR, 1500, 0.707); hp2.setHighPass (SR, 1500, 0.707);
            for (int i = 0; i < sig.getNumSamples(); ++i) sig.addSample (c, i, 1.6f * hp2.process (hp1.process (drums.getSample (c, i)))); }
        for (int t : { (int) ac::soft, (int) ac::tape, (int) ac::tube })
        {
            double r[3]; const int steps[3] = { 1, 3, ac::protectAuto };
            for (int k = 0; k < 3; ++k)
            {
                auto p = makeProc (true, [&] (APP& a) { setParam (a, "type", (float) t); setParam (a, "amount", 6); setParam (a, "protect", (float) steps[k]); });
                r[k] = bassHarmonicsDb (run (*p, sig), f0);
            }
            std::printf ("  %-5s 55 Hz bass: 60 Hz %6.1f | 120 Hz %6.1f | AUTO %6.1f dB\n", ac::typeName (t), r[0], r[1], r[2]);
            // (60 Hz used to be a trap here; since the room is measured on the real low band it isn't for
            // SOFT/TUBE any more, but it still is for TAPE.)
            check (r[2] <= r[1] + 1.0 && r[2] <= r[0] + 4.0 && r[2] < -45.0, juce::String (ac::typeName (t)) + ": AUTO keeps a 55 Hz bass as clean as 120 Hz, never worse than 60 Hz, under -45 dB ("
                   + juce::String (r[2], 1) + " dB)");
            if (t == ac::tape) check (r[2] < r[0] - 6.0, "TAPE: AUTO far cleaner than 60 Hz on a 55 Hz bass");
        }
    }
    // A saved session keeps its AUTO crossover; LEARN listens again
    {
        auto a = makeProc (true);
        run (*a, withDrums (bassLine (5.0, { 41.2 }, 1.0)));
        const float hzA = a->getAutoProtectHz();
        juce::MemoryBlock mb; a->getStateInformation (mb);
        auto b = makeProc (true, [&] (APP& x) { x.setStateInformation (mb.getData(), (int) mb.getSize()); });
        check (std::abs (b->getAutoProtectHz() - hzA) < 0.01f, "restored session keeps its AUTO crossover (" + juce::String (hzA, 1) + " Hz)");
        b->requestLearn();
        run (*b, withDrums (bassLine (5.0, { 73.4 }, 1.0)));
        check (b->getAutoProtectHz() > 130.0f, "LEARN on a higher bass moves AUTO up (" + juce::String (b->getAutoProtectHz(), 1) + " Hz)");
    }
}

// ── Quiet intro: gain staging can't blow up the first seconds ────────────────
static void introTests()
{
    std::printf ("\n== Quiet intro: 6 s of drums at -30 dBFS, then the drop at -6 dBFS (Lite, AMOUNT 3) ==\n");
    auto drums = drumLoop (6.0);
    const float pk = samplePeak (drums);
    Buf sig (2, (int) (12 * SR));
    for (int c = 0; c < 2; ++c)
    {
        sig.copyFrom (c, 0, drums, c, 0, drums.getNumSamples());
        sig.applyGain (c, 0, drums.getNumSamples(), juce::Decibels::decibelsToGain (-30.0f) / pk);
        sig.copyFrom (c, drums.getNumSamples(), drums, c, 0, drums.getNumSamples());
        sig.applyGain (c, drums.getNumSamples(), drums.getNumSamples(), juce::Decibels::decibelsToGain (-6.0f) / pk);
    }
    auto p = makeProc (false);
    std::vector<float> st;
    Buf out (2, sig.getNumSamples()), blk (2, BS); juce::MidiBuffer m;
    for (int pos = 0; pos + BS <= sig.getNumSamples(); pos += BS)
    {
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, sig, c, pos, BS);
        p->processBlock (blk, m);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, BS);
        st.push_back (p->getStagingDb());
    }
    auto at = [&] (double t) { return st[(size_t) juce::jmin ((int) st.size() - 1, (int) (t * SR / BS))]; };
    float maxRise = 0.0f;
    for (size_t i = 1; i < st.size(); ++i) maxRise = juce::jmax (maxRise, (st[i] - st[i - 1]) * (float) (SR / BS));
    auto segLufs = [&] (double a, double b) { Buf seg (2, (int) ((b - a) * SR)); for (int c = 0; c < 2; ++c) seg.copyFrom (c, 0, out, c, (int) (a * SR), seg.getNumSamples()); return lufs (seg); };
    const double introL = segLufs (0.5, 3.0), dropL = segLufs (7.0, 11.0);
    std::printf ("  gain staging at 0.5 / 1 / 2 / 3 / 5.9 s: %+.1f / %+.1f / %+.1f / %+.1f / %+.1f dB, after the drop %+.1f dB; fastest rise %.1f dB/s\n",
                 at (0.5), at (1.0), at (2.0), at (3.0), at (5.9), at (11.0), maxRise);
    std::printf ("  out: intro (0.5-3 s) %.1f LUFS, drop %.1f LUFS: intro %.1f dB under the drop (dry: 24 dB)\n", introL, dropL, dropL - introL);
    check (at (3.0) <= 6.0f + 18.0f * 3.0f / 16.0f + 0.1f, "first 3 s: boost capped (" + juce::String (at (3.0), 1) + " dB, was +24)");
    check (dropL - introL >= 12.0, "quiet intro stays well under the drop (" + juce::String (dropL - introL, 1) + " dB)");
    check (maxRise <= 12.1f, "gain staging never jumps up (fastest rise " + juce::String (maxRise, 1) + " dB/s)");
    check (std::abs (at (11.0) - 5.7f) < 0.3f, "the drop still lands at the ceiling (" + juce::String (at (11.0), 1) + " dB staging)");

    // Normal material (already loud) is staged within a second, smoothly
    auto q = makeProc (false);
    Buf loud (drums); loud.applyGain (juce::Decibels::decibelsToGain (-6.0f) / pk);
    run (*q, loud, (int) (1.0 * SR));
    const float want = ac::gate::liteCeilingDb - q->getInputPeakDb();
    check (std::abs (q->getStagingDb() - want) < 0.3f, "a loud track reaches full gain staging within 1 s (" + juce::String (q->getStagingDb(), 2) + " of " + juce::String (want, 2) + " dB)");
}

// ── Type character ───────────────────────────────────────────────────────────
static void typeTests()
{
    std::printf ("\n== Type character: 100 Hz sine pushed 6 dB into the clipper (4x); harmonic levels vs fundamental ==\n");
    const double f0 = std::round (100.0 / (SR / 65536)) * (SR / 65536);
    auto sig = sine (2.5, f0, 0.5f);
    for (int t = 0; t < ac::numTypes; ++t)
    {
        auto p = makeProc (true, [&] (APP& a) { setParam (a, "type", (float) t); setParam (a, "amount", 6); setParam (a, "protect", 0); });   // the bare curve
        auto out = run (*p, sig);
        const int order = 16, N = 1 << order; juce::dsp::FFT fft (order);
        std::vector<float> d ((size_t) N * 2, 0.0f);
        const int st = out.getNumSamples() - N;
        for (int i = 0; i < N; ++i) d[(size_t) i] = (float) (out.getSample (0, st + i) * (0.5 - 0.5 * std::cos (twoPi * i / N)));
        fft.performFrequencyOnlyForwardTransform (d.data());
        const double bin = SR / N;
        auto band = [&] (double f) { double s = 0; for (int k = (int) (f / bin) - 3; k <= (int) (f / bin) + 3; ++k) s += (double) d[(size_t) k] * d[(size_t) k]; return s; };
        const double fund = band (f0);
        std::printf ("  %-5s H2 %6.1f  H3 %6.1f  H5 %6.1f  H7 %6.1f dB   out peak %5.2f dB\n", ac::typeName (t),
                     10 * std::log10 (band (2 * f0) / fund), 10 * std::log10 (band (3 * f0) / fund), 10 * std::log10 (band (5 * f0) / fund), 10 * std::log10 (band (7 * f0) / fund),
                     juce::Decibels::gainToDecibels (samplePeak (out, (int) SR)));
        if (t == ac::tube) check (band (2 * f0) / fund > 1e-4, "TUBE makes a real 2nd harmonic (even, above -40 dB)");
        if (t != ac::tube) check (band (2 * f0) / fund < 1e-5, juce::String (ac::typeName (t)) + " is symmetric (no 2nd harmonic)");
    }
}

// ── Clicks ───────────────────────────────────────────────────────────────────
struct HF { ac::Biquad a, b; HF() { a.setHighPass (SR, 2500, 0.707); b.setHighPass (SR, 2500, 0.707); } float operator() (float x) { return b.process (a.process (x)); } };

static void clickTests()
{
    std::printf ("\n== Clicks when switching (80 Hz tone at -8 dBFS, light clipping; HF > 2.5 kHz right after each switch vs steady state;\n"
                 "   pass = under steady + 6 dB, or under -66 dBFS = more than 58 dB below the tone) ==\n");
    struct Cat { const char* name; std::function<void (APP&, int)> change; };
    std::vector<Cat> cats = {
        { "amount 0/12",   [] (APP& p, int k) { setParam (p, "amount", k % 2 ? 12.0f : 0.0f); } },
        { "type",          [] (APP& p, int k) { setParam (p, "type", (float) (k % 4)); } },
        { "ceiling",       [] (APP& p, int k) { setParam (p, "ceiling", k % 2 ? -12.0f : 0.0f); } },
        { "protect steps", [] (APP& p, int k) { setParam (p, "protect", (float) (k % 6)); } },
        { "quality steps", [] (APP& p, int k) { setParam (p, "quality", (float) (k % 4)); } },
        { "mix 0/100",     [] (APP& p, int k) { setParam (p, "mix", k % 2 ? 0.0f : 100.0f); } },
        { "true peak",     [] (APP& p, int k) { setParam (p, "truepeak", (float) (k % 2)); } },
        { "match",         [] (APP& p, int k) { setParam (p, "match", (float) (k % 2)); } },
        { "Pro on/off",    [] (APP& p, int k) { setPro (p, k % 2 ? hz::LicenceInfo::none : hz::LicenceInfo::pro); } },
        { "LEARN",         [] (APP& p, int) { p.requestLearn(); } },
    };
    const double secs = 6.0; const int total = (int) (secs * SR);
    auto tone = sine (secs, 80.0, 0.4f);
    std::printf ("%-14s %12s %12s %8s\n", "switch", "steady dB", "switch dB", "click");
    for (auto& cat : cats)
    {
        auto p = makeProc (true, [&] (APP& a) { if (juce::String (cat.name) == "Pro on/off") { setParam (a, "type", 2); setParam (a, "ceiling", -3); setParam (a, "quality", 1); setParam (a, "protect", 3); setParam (a, "mix", 70); } });
        HF hf; float steady = 0, sw = 0;
        const int every = (int) (0.25 * SR) / BS * BS, after = (int) (0.06 * SR);
        int k = 0, lastChange = -1000000;
        Buf blk (2, BS); juce::MidiBuffer m;
        for (int pos = 0; pos + BS <= total; pos += BS)
        {
            if (pos >= (int) (1.0 * SR) && pos % every == 0) { cat.change (*p, ++k); lastChange = pos; }
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, tone, c, pos, BS);
            p->processBlock (blk, m);
            for (int i = 0; i < BS; ++i)
            {
                const float y = std::abs (hf (blk.getSample (0, i)));
                if (pos + i < (int) (0.8 * SR)) continue;
                if (pos + i - lastChange < after) sw = juce::jmax (sw, y);
                else steady = juce::jmax (steady, y);
            }
        }
        const float sDb = juce::Decibels::gainToDecibels (steady, -150.0f), wDb = juce::Decibels::gainToDecibels (sw, -150.0f);
        // A click is a jump: broadband HF far above the steady state. Anything under -66 dBFS here is
        // more than 58 dB below the tone and inaudible, so it passes too.
        const bool ok = wDb < sDb + 6.0f || wDb < -66.0f;
        std::printf ("%-14s %12.1f %12.1f %8s\n", cat.name, sDb, wDb, ok ? "none" : "CLICK");
        check (ok, juce::String ("no click: ") + cat.name);
    }
}

// ── State ────────────────────────────────────────────────────────────────────
static void stateTests (const Buf& mix)
{
    std::printf ("\n== Save -> new instance -> restore ==\n");
    auto a = makeProc (true);
    run (*a, mix, (int) (3 * SR));
    setParam (*a, "amount", 7.5f); setParam (*a, "type", 3); setParam (*a, "ceiling", -1.5f); setParam (*a, "protect", 2);
    setParam (*a, "quality", 3); setParam (*a, "mix", 64); setParam (*a, "truepeak", 1); setParam (*a, "match", 1);
    juce::MemoryBlock mb; a->getStateInformation (mb);
    const float peakA = a->getInputPeakDb();

    auto b = std::make_unique<APP>(); setPro (*b, hz::LicenceInfo::pro);
    b->setStateInformation (mb.getData(), (int) mb.getSize());
    check (b->hasHeardSignal() && std::abs (b->getInputPeakDb() - peakA) < 0.01f, "learned input peak restored (" + juce::String (peakA, 2) + " dB), no re-learn");
    auto val = [&] (const char* id) { return b->apvts.getRawParameterValue (id)->load(); };
    check (std::abs (val ("amount") - 7.5f) < 0.05f && (int) val ("type") == 3 && std::abs (val ("ceiling") + 1.5f) < 0.05f && (int) val ("protect") == 2
           && (int) val ("quality") == 3 && std::abs (val ("mix") - 64) < 0.05f && val ("truepeak") > 0.5f && val ("match") > 0.5f, "every parameter survives");
    juce::MemoryBlock mb2; b->getStateInformation (mb2);
    check (mb2 == mb, "saving the restored session gives identical bytes");

    auto c = std::make_unique<APP>(); setPro (*c, hz::LicenceInfo::pro);
    c->setStateInformation (mb.getData(), (int) mb.getSize());
    b->setRateAndBufferSizeDetails (SR, BS); b->prepareToPlay (SR, BS);
    c->setRateAndBufferSizeDetails (SR, BS); c->prepareToPlay (SR, BS);
    for (int i = 0; i < 2; ++i) { std::unique_ptr<juce::AudioProcessorEditor> ed (b->createEditor()); }
    const auto o1 = run (*b, mix, (int) (3 * SR)), o2 = run (*c, mix, (int) (3 * SR));
    check (maxDiff (o1, o2) < 1e-7f, "two restored instances sound identical (window opened/closed on one)");
    // the restored instance starts at full gain staging straight away (no fade-in / re-learn ramp)
    auto fresh = makeProc (true, [&] (APP& x) { x.setStateInformation (mb.getData(), (int) mb.getSize()); });
    setParam (*fresh, "match", 0);
    const auto first = run (*fresh, mix, (int) (0.3 * SR));
    check (samplePeak (first) > juce::Decibels::decibelsToGain (-1.5f) * 0.9f, "restored session is at full level from the first block");

    b->requestLearn();
    run (*b, mix, BS * 4);
    check (b->isListening() || b->hasHeardSignal(), "LEARN listens again");
    run (*b, mix, (int) ((APP::learnSecs + 0.5) * SR));
    check (b->hasHeardSignal() && ! b->isListening(), "and picks the peak back up after " + juce::String (APP::learnSecs, 0) + " s of signal (" + juce::String (b->getInputPeakDb(), 2) + " dB)");
}

// ── Lite gate ────────────────────────────────────────────────────────────────
static void liteTests (const Buf& drums)
{
    std::printf ("\n== Lite can't use Pro features ==\n");
    const int len = (int) (4 * SR);
    auto ref = makeProc (false, [] (APP& a) { setParam (a, "amount", 6); });
    const auto refOut = run (*ref, drums, len);
    auto proSettings = [] (APP& p, int type, int os)
    {
        setParam (p, "amount", 6); setParam (p, "type", (float) type); setParam (p, "ceiling", -6); setParam (p, "protect", 4);
        setParam (p, "quality", (float) os); setParam (p, "mix", 40); setParam (p, "truepeak", 1);
    };
    for (int type : { (int) ac::soft, (int) ac::hard, (int) ac::tape, (int) ac::tube })
        for (int os : { 0, 3 })
        {
            auto lite = makeProc (false, [&] (APP& a) { proSettings (a, type, os); });
            const float d = maxDiff (run (*lite, drums, len), refOut);
            check (d == 0.0f, juce::String ("Lite + automation to ") + ac::typeName (type) + " / ceiling -6 / protect 150 / " + juce::String (ac::osFactors[os]) + "x / mix 40 / true peak = plain Lite (max diff " + juce::String (d) + ")");
        }
    // automation arriving mid-play
    {
        auto lite = makeProc (false, [] (APP& a) { setParam (a, "amount", 6); });
        Buf out (2, len);
        auto first = run (*lite, drums, len / 2);
        proSettings (*lite, ac::hard, 3);
        auto second = run (*lite, drums, len - len / 2, len / 2);
        for (int c = 0; c < 2; ++c) { out.copyFrom (c, 0, first, c, 0, len / 2); out.copyFrom (c, len / 2, second, c, 0, len - len / 2); }
        check (maxDiff (out, refOut) == 0.0f, "Pro automation arriving mid-play in Lite changes nothing");
    }
    {
        auto pro = makeProc (true, [&] (APP& a) { proSettings (a, ac::tube, 3); });
        juce::MemoryBlock mb; pro->getStateInformation (mb);
        auto lite = std::make_unique<APP>(); setPro (*lite, hz::LicenceInfo::none);
        lite->setStateInformation (mb.getData(), (int) mb.getSize());
        lite->setRateAndBufferSizeDetails (SR, BS); lite->prepareToPlay (SR, BS);
        auto refFresh = makeProc (false, [] (APP& a) { setParam (a, "amount", 6); });
        const auto r2 = run (*refFresh, drums, len);
        const float d = maxDiff (run (*lite, drums, len), r2);
        check (d == 0.0f, "Pro session opened in Lite falls back to plain Lite (max diff " + juce::String (d) + ")");
        auto ex = std::make_unique<APP>(); setPro (*ex, hz::LicenceInfo::trialExpired);
        ex->setStateInformation (mb.getData(), (int) mb.getSize());
        ex->setRateAndBufferSizeDetails (SR, BS); ex->prepareToPlay (SR, BS);
        check (maxDiff (run (*ex, drums, len), r2) == 0.0f, "expired trial = Lite");
        auto tr = std::make_unique<APP>(); setPro (*tr, hz::LicenceInfo::trial);
        tr->setStateInformation (mb.getData(), (int) mb.getSize());
        tr->setRateAndBufferSizeDetails (SR, BS); tr->prepareToPlay (SR, BS);
        check (maxDiff (run (*tr, drums, len), r2) > 0.01f, "sanity: the same settings DO change the sound with a trial");
    }
    {
        auto a0 = makeProc (false, [] (APP& a) { setParam (a, "amount", 2); });
        check (maxDiff (run (*a0, drums, len), refOut) > 0.01f, "AMOUNT works in Lite");
        auto m = makeProc (false, [] (APP& a) { setParam (a, "amount", 6); setParam (a, "match", 1); });
        check (maxDiff (run (*m, drums, len), refOut) > 0.01f, "MATCH works in Lite");
    }
}

// ── CPU ──────────────────────────────────────────────────────────────────────
static void cpuTests()
{
    std::printf ("\n== CPU (10 s stereo at 48 kHz, this machine, one core) ==\n");
    auto sig = mixBus (10.0);
    for (int os = 0; os < 4; ++os)
        for (int prot : { 0, 3 })
        {
            auto p = makeProc (true, [&] (APP& a) { setParam (a, "quality", (float) os); setParam (a, "protect", (float) prot); setParam (a, "truepeak", 1); });
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            run (*p, sig, -1, 0, 512);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            std::printf ("  QUALITY %dx, protect %s, true peak on: %6.1f ms = %.2f %% of real time\n", ac::osFactors[os], prot ? "on " : "off", ms, ms / 100.0);
        }
}

// ── UI ───────────────────────────────────────────────────────────────────────
static void snapshots (const juce::File& outDir, const Buf& mix)
{
    std::printf ("\n== UI snapshots ==\n");
    auto snap = [&] (const char* name, int licKind, const char* panel, std::function<void (APP&)> setup, bool feed = true)
    {
        auto p = std::make_unique<APP>(); setPro (*p, licKind);
        if (setup) setup (*p);
        p->setRateAndBufferSizeDetails (SR, BS); p->prepareToPlay (SR, BS);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        auto* t = dynamic_cast<AutoClipProAudioProcessorEditor*> (ed.get());
        if (feed)
            for (int i = 0; i < 100; ++i) { run (*p, mix, (int) (0.03 * SR), i * (int) (0.03 * SR)); if (t) t->timerCallback(); }
        else if (t) for (int i = 0; i < 5; ++i) t->timerCallback();
        if (panel != nullptr)
            for (auto* c : ed->getChildren())
                if (auto* pp = dynamic_cast<ProPanel*> (c)) pp->show (panel);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::FileOutputStream fo (outDir.getChildFile (name));
        fo.setPosition (0); fo.truncate();
        juce::PNGImageFormat().writeImageToStream (img, fo);
        float mxPre = 0, mxOut = 0; for (auto& col : p->scope) { mxPre = juce::jmax (mxPre, col.pre.load()); mxOut = juce::jmax (mxOut, col.out.load()); }
        std::printf ("  %s  (input peak %.1f dB, louder %.1f dB, display: in up to %+.1f dB, out up to %+.1f dB vs ceiling)\n", name, p->getInputPeakDb(), p->louderDb.load(),
                     juce::Decibels::gainToDecibels (mxPre), juce::Decibels::gainToDecibels (mxOut));
    };
    snap ("ui-1-lite.png", hz::LicenceInfo::none, nullptr, [] (APP& p) { setParam (p, "amount", 6); });
    snap ("ui-2-lite-pro-panel.png", hz::LicenceInfo::none, "HARD IS A PRO CLIP TYPE", [] (APP& p) { setParam (p, "amount", 6); });
    snap ("ui-3-lite-match.png", hz::LicenceInfo::none, nullptr, [] (APP& p) { setParam (p, "amount", 6); setParam (p, "match", 1); });
    snap ("ui-4-trial.png", hz::LicenceInfo::trial, nullptr, [] (APP& p) { setParam (p, "amount", 8); setParam (p, "type", 1); setParam (p, "quality", 3); });
    snap ("ui-5-trial-ended.png", hz::LicenceInfo::trialExpired, nullptr, [] (APP& p) { setParam (p, "amount", 6); setParam (p, "type", 2); setParam (p, "ceiling", -3); });
    snap ("ui-6-trial-ended-panel.png", hz::LicenceInfo::trialExpired, "", nullptr);
    snap ("ui-7-pro.png", hz::LicenceInfo::pro, nullptr, [] (APP& p)
          { setParam (p, "amount", 7); setParam (p, "type", 3); setParam (p, "ceiling", -1); setParam (p, "protect", 3); setParam (p, "quality", 3); setParam (p, "mix", 85); setParam (p, "truepeak", 1); });
    snap ("ui-8-pro-panel.png", hz::LicenceInfo::pro, "", nullptr);
    snap ("ui-9-waiting.png", hz::LicenceInfo::none, nullptr, nullptr, false);
    // Licence states (Launch prep renders)
    auto saved = [] (APP& p) { setParam (p, "amount", 6); setParam (p, "type", 2); setParam (p, "ceiling", -3); setParam (p, "quality", 3); };
    snap ("licence-1-free.png", hz::LicenceInfo::none, nullptr, [] (APP& p) { setParam (p, "amount", 6); });
    snap ("licence-2-free-panel.png", hz::LicenceInfo::none, "", nullptr);
    snap ("licence-3-trial.png", hz::LicenceInfo::trial, nullptr, [] (APP& p) { setParam (p, "amount", 8); setParam (p, "type", 1); });
    snap ("licence-4-trial-panel.png", hz::LicenceInfo::trial, "", nullptr);
    snap ("licence-5-trial-ended.png", hz::LicenceInfo::trialExpired, nullptr, [] (APP& p) { setParam (p, "amount", 6); });
    snap ("licence-6-trial-ended-panel.png", hz::LicenceInfo::trialExpired, "", nullptr);
    snap ("licence-7-licensed.png", hz::LicenceInfo::pro, nullptr, saved);
    snap ("licence-8-licensed-panel.png", hz::LicenceInfo::pro, "", nullptr);
    snap ("licence-9-saved-upgrade-settings.png", hz::LicenceInfo::trialExpired, nullptr, saved);
}

// ── Sound examples from real stems ───────────────────────────────────────────
static Buf loadMono (const juce::File& f, double& rate)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) { std::printf ("  can't read %s\n", f.getFullPathName().toRawUTF8()); return {}; }
    rate = r->sampleRate;
    Buf x (2, (int) r->lengthInSamples); r->read (&x, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1) x.copyFrom (1, 0, x, 0, 0, x.getNumSamples());
    return x;
}

static int examples (const juce::File& od, const juce::File& stems)
{
    od.createDirectory();
    double rate = 44100;
    auto bass = loadMono (stems.getChildFile ("bass.wav"), rate), groove = loadMono (stems.getChildFile ("groove.wav"), rate),
         hat = loadMono (stems.getChildFile ("hat.wav"), rate), synth = loadMono (stems.getChildFile ("synth.wav"), rate);
    if (bass.getNumSamples() == 0 || groove.getNumSamples() == 0) return 1;
    SR = rate;
    const int n = juce::jmin (bass.getNumSamples(), groove.getNumSamples(), hat.getNumSamples(), synth.getNumSamples());
    Buf drums (2, n), mix (2, n);
    for (int c = 0; c < 2; ++c)
    {
        drums.copyFrom (c, 0, groove, c, 0, n); drums.addFrom (c, 0, hat, c, 0, n);
        mix.copyFrom (c, 0, drums, c, 0, n); mix.addFrom (c, 0, bass, c, 0, n); mix.addFrom (c, 0, synth, c, 0, n);
    }
    // Dry files peak at -1 dBFS (a typical unmastered bounce).
    for (auto* b : { &drums, &mix }) b->applyGain (juce::Decibels::decibelsToGain (-1.0f) / samplePeak (*b));

    struct V { const char* label; int type; int prot; bool pro; };
    const V vs[] = { { "1-SOFT (Lite)", ac::soft, 0, false }, { "2-HARD", ac::hard, 0, true }, { "3-TAPE", ac::tape, 0, true },
                     { "4-TUBE", ac::tube, 0, true }, { "5-HARD + LOW PROTECT 120 Hz", ac::hard, 3, true },
                     { "6-HARD + LOW PROTECT AUTO", ac::hard, ac::protectAuto, true } };
    const float amount = 6.0f;
    std::printf ("Sound examples, AMOUNT %.0f dB, loudness-matched to the dry file (so you hear tone, not volume)\n", amount);
    for (auto [name, dry] : { std::pair<const char*, Buf*> { "drums", &drums }, { "full-mix", &mix } })
    {
        const double dryL = lufs (*dry, 0, rate);
        writeWav (od.getChildFile (juce::String (name) + "-0-DRY.wav"), *dry, rate);
        std::printf ("  %-9s DRY  %.1f LUFS  peak %.1f dBFS\n", name, dryL, juce::Decibels::gainToDecibels (samplePeak (*dry)));
        for (const auto& v : vs)
        {
            auto p = makeProc (v.pro, [&] (APP& a) { setParam (a, "amount", amount); setParam (a, "type", (float) v.type); setParam (a, "protect", (float) v.prot); });
            run (*p, *dry);                        // first pass: learns the loudest peak (like the second playback in a DAW)
            auto out = run (*p, *dry);
            // remove the plugin latency so the files line up with DRY
            const int L = p->getLatencySamples();
            Buf al (2, n); al.clear();
            for (int c = 0; c < 2; ++c) al.copyFrom (c, 0, out, c, L, n - L);
            const double L1 = lufs (al, 0, rate);
            const float pk = samplePeak (al);
            if (v.type == ac::soft)
                writeWav (od.getChildFile (juce::String (name) + "-1b-SOFT (Lite) as it comes out, louder.wav"), al, rate);
            al.applyGain ((float) juce::Decibels::decibelsToGain (dryL - L1));
            writeWav (od.getChildFile (juce::String (name) + "-" + v.label + ".wav"), al, rate);
            std::printf ("  %-9s %-28s out %.1f LUFS (%+.1f LU louder than dry) peak %.2f dBFS -> matched to %.1f LUFS%s\n",
                         name, v.label, L1, L1 - dryL, juce::Decibels::gainToDecibels (pk), lufs (al, 0, rate),
                         v.prot == ac::protectAuto ? (" (AUTO picked " + juce::String (p->getAutoProtectHz(), 0) + " Hz)").toRawUTF8() : "");
        }
    }
    return 0;
}



// ── LEARN ────────────────────────────────────────────────────────────────────
// Plays `sig` through `p` block by block (the message thread's part of LEARN done after every block, like the
// plug-in's async update). Returns the output; optionally the highest CLIP reading from sample `clipFrom`.
static Buf play (APP& p, const Buf& sig, int len, float* maxClip = nullptr, int clipFrom = 0, std::vector<float>* staging = nullptr)
{
    Buf out (2, len), blk (2, BS); juce::MidiBuffer m;
    const int n = sig.getNumSamples();
    for (int pos = 0; pos < len; pos += BS)
    {
        const int k = juce::jmin (BS, len - pos);
        blk.setSize (2, k, false, false, true);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < k; ++i) blk.setSample (c, i, sig.getSample (juce::jmin (c, sig.getNumChannels() - 1), (pos + i) % n));
        p.processBlock (blk, m);
        p.applyLearnedAmount();
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, k);
        if (maxClip && pos >= clipFrom) *maxClip = juce::jmax (*maxClip, p.clipDb.load());
        if (staging) staging->push_back (p.getStagingDb());
    }
    return out;
}

// Distortion residual: what's left of `y` after taking away `ref` (the same chain set so it doesn't clip:
// identical latency and oversampling filters) times the best-matching gain over each 10 ms, vs the signal, dB.
// The 10 ms gain follows level changes (the safety limiter riding a peak, a fader) but not the clipping
// itself, which reshapes the waveform within a cycle: the same measure the reference renders use.
static double residualDb (const Buf& y, const Buf& ref, int from, int to)
{
    const int W = juce::jmax (1, (int) (0.005 * SR)), n = to - from;
    std::vector<double> yr ((size_t) n + 1, 0.0), rr ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
    {
        double a = 0, b = 0;
        for (int c = 0; c < 2; ++c) { a += (double) y.getSample (c, from + i) * ref.getSample (c, from + i); b += (double) ref.getSample (c, from + i) * ref.getSample (c, from + i); }
        yr[(size_t) i + 1] = yr[(size_t) i] + a; rr[(size_t) i + 1] = rr[(size_t) i] + b;
    }
    double e = 0, sgl = 0;
    for (int i = 0; i < n; ++i)
    {
        const int a = juce::jmax (0, i - W), b = juce::jmin (n, i + W + 1);
        const double den = rr[(size_t) b] - rr[(size_t) a];
        const double g = den > 1e-20 ? (yr[(size_t) b] - yr[(size_t) a]) / den : 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const double r = g * ref.getSample (c, from + i), d = y.getSample (c, from + i) - r;
            e += d * d; sgl += r * r;
        }
    }
    return 10.0 * std::log10 (e / (sgl + 1e-30) + 1e-30);
}

static void learnTests()
{
    std::printf ("\n== LEARN: listens %.0f s of signal, then sets the level and a clean AMOUNT ==\n", APP::learnSecs);
    {
        auto p = makeProc (true);
        check (std::abs (p->apvts.getRawParameterValue ("amount")->load() - ac::amountDefaultDb) < 1e-4f && ac::amountDefaultDb <= 2.0f,
               "AMOUNT starts conservative (" + juce::String (ac::amountDefaultDb, 1) + " dB)");
    }
    // progress, silence doesn't count, the level holds still while it listens
    {
        auto mix = mixBus (12.0);
        auto p = makeProc (true);
        play (*p, mix, (int) (4 * SR));                        // already staged
        const float before = p->getStagingDb();
        Buf withGap (2, (int) (2 * SR) + mix.getNumSamples()); withGap.clear();
        for (int c = 0; c < 2; ++c) withGap.copyFrom (c, (int) (2 * SR), mix, c, 0, mix.getNumSamples());
        p->requestLearn();
        std::vector<float> st;
        play (*p, withGap, (int) (2 * SR), nullptr, 0, &st);
        const float progSilent = p->getLearnProgress();
        Buf rest (2, mix.getNumSamples()); for (int c = 0; c < 2; ++c) rest.copyFrom (c, 0, mix, c, 0, mix.getNumSamples());
        play (*p, rest, (int) (3 * SR), nullptr, 0, &st);
        const float progHalf = p->getLearnProgress(); const bool listening = p->isListening();
        float drift = 0; for (float v : st) drift = juce::jmax (drift, std::abs (v - before));
        play (*p, rest, (int) ((APP::learnSecs - 3.0 + 0.4) * SR), nullptr, 0, nullptr);
        std::printf ("  progress after 2 s of silence %.2f, after 3 s of music %.2f; staging moved %.2f dB while listening; learned AMOUNT %.1f dB\n",
                     progSilent, progHalf, drift, p->getLearnedAmountDb());
        check (progSilent < 0.05f, "silence doesn't count towards LEARN");
        check (listening && std::abs (progHalf - 3.0f / (float) APP::learnSecs) < 0.06f, "LEARN shows its progress (" + juce::String (progHalf * 100, 0) + " % after 3 s)");
        check (drift < 0.05f, "the level holds still while LEARN listens (no dip, no jump)");
        check (! p->isListening() && p->getLearnCount() == 1, "LEARN finishes after " + juce::String (APP::learnSecs, 0) + " s of music");
        check (std::abs (p->apvts.getRawParameterValue ("amount")->load() - p->getLearnedAmountDb()) < 0.06f, "LEARN puts its AMOUNT on the dial");
        check (p->getLearnedAmountDb() <= APP::learnMaxDb, "learned AMOUNT within " + juce::String (APP::learnMaxDb, 0) + " dB");
        play (*p, rest, (int) (3 * SR));
        check (p->clipDistDb.load() < APP::distCleanDb, "after LEARN the clip meter reads clean (" + juce::String (p->clipDistDb.load(), 1) + " dB)");
    }
    // LEARN in a quiet intro, then the drop comes in 24 dB louder: it meets the clipper at the same AMOUNT
    {
        auto drums = drumLoop (8.0); const float pk = samplePeak (drums);
        Buf sig (2, 2 * drums.getNumSamples());
        for (int c = 0; c < 2; ++c)
        {
            sig.copyFrom (c, 0, drums, c, 0, drums.getNumSamples());
            sig.applyGain (c, 0, drums.getNumSamples(), juce::Decibels::decibelsToGain (-30.0f) / pk);
            sig.copyFrom (c, drums.getNumSamples(), drums, c, 0, drums.getNumSamples());
            sig.applyGain (c, drums.getNumSamples(), drums.getNumSamples(), juce::Decibels::decibelsToGain (-6.0f) / pk);
        }
        auto p = makeProc (true);
        p->requestLearn();
        float clipDrop = 0;
        play (*p, sig, sig.getNumSamples(), &clipDrop, drums.getNumSamples());
        std::printf ("  LEARN in a -30 dBFS intro, then a -6 dBFS drop: AMOUNT %.1f dB, highest CLIP after the drop %.2f dB\n", p->getLearnedAmountDb(), clipDrop);
        check (clipDrop <= p->getLearnedAmountDb() + 0.3f, "a louder drop after LEARN doesn't slam the clipper (" + juce::String (clipDrop, 2) + " dB)");
    }
    // the meter goes hot when you push it
    {
        auto p = makeProc (true, [] (APP& a) { setParam (a, "amount", 12); });
        play (*p, drumLoop (6.0), (int) (6 * SR));
        check (p->clipDistDb.load() > APP::distHeavyDb, "AMOUNT 12 on drums: the clip meter reads hot (" + juce::String (p->clipDistDb.load(), 1) + " dB)");
    }
}

// LEARN on Sam's real stems at typical Logic levels. The residual limit comes from the reference renders
// (README in the comparison folder): FabFilter Pro-L 2 (Modern, 4x) pushed for +2 dB of loudness on the
// drum loop leaves a -33 dB residual on this same measure; LEARN has to be at least that clean on every
// stem, at every level, and may never clip the loudest peak by more than learnMaxDb.
static void learnStemTests()
{
    const char* env = std::getenv ("AUTOCLIP_STEMS");
    const juce::File root = env ? juce::File (juce::String (env)) : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("99hertz/ads/how-to-make/out/stems");
    const auto g = root.getChildFile ("gta6-01-stussy-ninety"), h = root.getChildFile ("90s-piano-house-15");
    std::printf ("\n== LEARN on real stems (Logic levels; pass = AMOUNT <= %.0f dB, CLIP <= AMOUNT + 0.2 dB, residual <= %.0f dB) ==\n", APP::learnMaxDb, -33.0);
    if (! g.getChildFile ("groove.wav").existsAsFile() || ! h.getChildFile ("piano.wav").existsAsFile())
    { std::printf ("  (stems not found in %s: skipped)\n", root.getFullPathName().toRawUTF8()); return; }
    const double keep = SR;
    double rate = 44100;
    auto ld = [&] (const juce::File& f) { return loadMono (f, rate); };
    std::vector<std::pair<juce::String, Buf>> src;
    auto groove = ld (g.getChildFile ("groove.wav")), hat = ld (g.getChildFile ("hat.wav")), bass = ld (g.getChildFile ("bass.wav")), synth = ld (g.getChildFile ("synth.wav"));
    auto piano = ld (h.getChildFile ("piano.wav")), pbass = ld (h.getChildFile ("bass.wav")), vocal = ld (h.getChildFile ("vocal.wav"));
    SR = rate;
    auto sum = [] (std::initializer_list<const Buf*> bs) { Buf o (**bs.begin()); bool first = true; for (auto* b : bs) { if (first) { first = false; continue; } for (int c = 0; c < 2; ++c) o.addFrom (c, 0, *b, c, 0, juce::jmin (o.getNumSamples(), b->getNumSamples())); } return o; };
    src = { { "gta groove", groove }, { "gta hat", hat }, { "gta bass", bass }, { "gta synth", synth }, { "gta drums (groove+hat)", sum ({ &groove, &hat }) },
            { "gta full mix", sum ({ &groove, &hat, &bass, &synth }) }, { "piano-house piano", piano }, { "piano-house bass", pbass },
            { "piano-house vocal", vocal }, { "piano-house mix", sum ({ &piano, &pbass, &vocal }) } };
    float worstRes = -200, worstOver = -100, worstAmt = 0;
    for (auto& [name, raw] : src)
    {
        const bool full = name.contains ("drums") || name.contains ("mix");
        for (float peakDb : { -18.0f, -12.0f, -6.0f })
        {
            if (! full && peakDb != -12.0f) continue;
            Buf sig (raw); sig.applyGain (juce::Decibels::decibelsToGain (peakDb) / samplePeak (raw));
            const int n = sig.getNumSamples();
            auto p = makeProc (true);                                          // Pro defaults, like Sam's session
            auto r = makeProc (true, [] (APP& a) { setParam (a, "type", 1); setParam (a, "amount", 0); setParam (a, "ceiling", 0); setParam (a, "protect", 0); });
            p->requestLearn(); r->requestLearn();
            float maxClip = 0;
            const auto y = play (*p, sig, 2 * n, &maxClip, n);                  // second time round the loop
            Buf ry (2, 2 * n), blk (2, BS); juce::MidiBuffer m;                // reference: same chain, no clipping (AMOUNT stays 0)
            for (int pos = 0; pos < 2 * n; pos += BS)
            {
                const int k = juce::jmin (BS, 2 * n - pos); blk.setSize (2, k, false, false, true);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < k; ++i) blk.setSample (c, i, sig.getSample (c, (pos + i) % n));
                r->processBlock (blk, m);
                for (int c = 0; c < 2; ++c) ry.copyFrom (c, pos, blk, c, 0, k);
            }
            const double res = residualDb (y, ry, n, 2 * n);
            Buf seg (2, n), dry (2, n);
            for (int c = 0; c < 2; ++c) { seg.copyFrom (c, 0, y, c, n, n); dry.copyFrom (c, 0, sig, c, 0, n); }
            const double gain = lufs (seg) - (lufs (dry) + (-0.3 - juce::Decibels::gainToDecibels (samplePeak (dry))));
            const float amt = p->getLearnedAmountDb();
            std::printf ("  %-24s %5.0f dBFS: AMOUNT %.1f dB, CLIP max %.2f dB, %+.2f dB louder than peak-normalised, residual %6.1f dB, meter %6.1f dB\n",
                         name.toRawUTF8(), peakDb, amt, maxClip, gain, res, p->clipDistDb.load());
            worstRes = juce::jmax (worstRes, (float) res); worstOver = juce::jmax (worstOver, maxClip - amt); worstAmt = juce::jmax (worstAmt, amt);
            check (amt <= APP::learnMaxDb && maxClip <= amt + 0.2f && res <= -33.0,
                   "LEARN on " + name + " at " + juce::String (peakDb, 0) + " dBFS: clean (" + juce::String (res, 1) + " dB, " + juce::String (amt, 1) + " dB AMOUNT)");
        }
    }
    std::printf ("  worst: residual %.1f dB, AMOUNT %.1f dB, CLIP over AMOUNT %+.2f dB\n", worstRes, worstAmt, worstOver);
    SR = keep;
}

// ── Render one file (for measurements against other plug-ins) ────────────────
// AutoClipProTest renderfile <in.wav> <out.wav> [peak=-12] [amount=3] [type=0] [protect=5] [quality=2] [ceiling=-0.3]
//                 [mix=100] [tp=0] [match=0] [pro=1] [learn=<sec>] [loops=2] [log=<csv>]
// The input is scaled to `peak` dBFS (sample peak), played `loops` times back to back (like a looping DAW
// region); LEARN is pressed at `learn` seconds. The output is latency-compensated, same length.
static int renderFile (int argc, char** argv)
{
    std::map<juce::String, juce::String> kv;
    for (int a = 4; a < argc; ++a) { const juce::String s (argv[a]); kv[s.upToFirstOccurrenceOf ("=", false, false)] = s.fromFirstOccurrenceOf ("=", false, false); }
    auto num = [&] (const char* k, double def) { return kv.count (k) ? kv[k].getDoubleValue() : def; };
    double rate = 44100;
    auto x = loadMono (juce::File (juce::String (argv[2])), rate);
    if (x.getNumSamples() == 0) return 1;
    SR = rate;
    if (kv.count ("peak")) x.applyGain (juce::Decibels::decibelsToGain ((float) num ("peak", -12)) / juce::jmax (1.0e-9f, samplePeak (x)));
    const int loops = (int) num ("loops", 2), n1 = x.getNumSamples(), n = n1 * loops;
    auto p = makeProc (num ("pro", 1) > 0.5, [&] (APP& a)
    {
        for (auto [k, id] : { std::pair<const char*, const char*> { "amount", "amount" }, { "type", "type" }, { "protect", "protect" }, { "quality", "quality" },
                              { "ceiling", "ceiling" }, { "mix", "mix" }, { "tp", "truepeak" }, { "match", "match" } })
            if (kv.count (k)) setParam (a, id, (float) num (k, 0));
    });
    const int L = p->getLatencySamples();
    const int learnAt = kv.count ("learn") ? (int) (num ("learn", 0) * rate) : -1;
    std::unique_ptr<juce::FileOutputStream> log;
    if (kv.count ("log")) { juce::File lf (kv["log"]); lf.deleteFile(); log = lf.createOutputStream(); }
    if (log) log->writeText ("t,staging,clip,ref,listening,amount,dist\n", false, false, nullptr);
    Buf out (2, n + L), blk (2, BS); juce::MidiBuffer m;
    for (int pos = 0; pos < n + L; pos += BS)
    {
        const int len = juce::jmin (BS, n + L - pos);
        blk.setSize (2, len, false, false, true);
        if (learnAt >= 0 && pos <= learnAt && learnAt < pos + BS) p->requestLearn();
        for (int c = 0; c < 2; ++c) for (int i = 0; i < len; ++i) blk.setSample (c, i, pos + i < n ? x.getSample (c, (pos + i) % n1) : 0.0f);
        p->processBlock (blk, m);
        p->applyLearnedAmount();                  // what the message thread does when LEARN finishes
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, len);
        if (log) log->writeText (juce::String (pos / rate, 4) + "," + juce::String (p->getStagingDb(), 3) + "," + juce::String (p->clipDb.load(), 3) + ","
                                 + juce::String (p->getInputPeakDb(), 3) + "," + juce::String ((int) p->isListening()) + ","
                                 + juce::String (p->apvts.getRawParameterValue ("amount")->load(), 2) + "," + juce::String (p->clipDistDb.load(), 2) + "\n", false, false, nullptr);
    }
    Buf al (2, n);
    for (int c = 0; c < 2; ++c) al.copyFrom (c, 0, out, c, L, n);
    writeWav (juce::File (juce::String (argv[3])), al, rate);
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 3 && juce::String (argv[1]) == "renderfile")
        return renderFile (argc, argv);
    if (argc > 3 && juce::String (argv[1]) == "examples")
        return examples (juce::File (juce::String (argv[2])), juce::File (juce::String (argv[3])));

    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();
    const bool quick = argc > 2 && juce::String (argv[2]) == "quick";

    const auto drums = drumLoop (12), mix = mixBus (12);
    latencyTests();
    if (! quick) ceilingTests();
    aliasTests();
    typeTests();
    loudnessTests();
    protectTests();
    protectAutoTests();
    introTests();
    learnTests();
    learnStemTests();
    clickTests();
    stateTests (mix);
    liteTests (drums);
    if (! quick) cpuTests();
    snapshots (outDir, mix);

    std::printf ("\n== Licence keys ==\n");
    for (int a = 2; a < argc; ++a)
    {
        const juce::String arg (argv[a]);
        if (! arg.containsChar ('=')) continue;
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false);
        const auto info = hz::Licence::verify (arg.fromFirstOccurrenceOf ("=", false, false), "autoclippro");
        const char* kinds[] = { "none", "trial", "pro", "trialExpired", "invalid" };
        std::printf ("  key %-14s -> %-12s daysLeft=%d\n", name.toRawUTF8(), kinds[(int) info.kind], info.daysLeft());
        hz::LicenceInfo::Kind want = hz::LicenceInfo::invalid;
        if (name == "pro") want = hz::LicenceInfo::pro;
        else if (name == "trial") want = hz::LicenceInfo::trial;
        else if (name == "expired") want = hz::LicenceInfo::trialExpired;
        else if (name == "all") want = hz::LicenceInfo::pro;
        check (info.kind == want, "key " + name + " -> " + kinds[(int) info.kind]);
    }

    hztest::runLicenceTests<APP> ("autoclippro", [] (APP& p) { return p.proUnlocked.load(); });
    hztest::renderLicenceStates<APP> ("autoclippro", outDir);
    hztest::runSavedSettingsTests<APP> ([] (APP& p)
    {
        setParam (p, "type", 2); setParam (p, "ceiling", -3); setParam (p, "quality", 3);
        setParam (p, "protect", 2); setParam (p, "mix", 70); setParam (p, "truepeak", 1);
    });
    {
        // and the audio still falls back to free: no licence + saved upgrade settings = SOFT, -0.3 dB, 4x, full mix
        APP p; p.setLicenceInfo ({});
        setParam (p, "type", 2); setParam (p, "ceiling", -3); setParam (p, "mix", 70);
        const auto e = p.resolve (p.proUnlocked.load());
        hztest::expect (p.usesUpgradeSettings() && e.type == ac::soft && std::abs (e.ceilingDb - ac::gate::liteCeilingDb) < 0.01f && e.mix == 1.0f,
                        "no licence + saved upgrade settings: flagged, plays as free");
    }
    hztest::finish();

    failures += hztest::failures;
    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
