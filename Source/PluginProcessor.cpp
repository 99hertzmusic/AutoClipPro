#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

using namespace ac;

static constexpr float capStartDb = 6.0f;            // the most a fresh instance boosts at first...
static constexpr double trustSecs = 16.0;            // ...rising to the full +24 dB over this much heard signal
static constexpr double stagingRiseDbPerSec = 12.0;   // a smooth ~0.5 s glide for a typical bounce, never a jump

AutoClipProAudioProcessor::AutoClipProAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AutoClipProState", createParameterLayout())
{
    pAmount   = apvts.getRawParameterValue ("amount");
    pType     = apvts.getRawParameterValue ("type");
    pCeiling  = apvts.getRawParameterValue ("ceiling");
    pProtect  = apvts.getRawParameterValue ("protect");
    pQuality  = apvts.getRawParameterValue ("quality");
    pMix      = apvts.getRawParameterValue ("mix");
    pTruePeak = apvts.getRawParameterValue ("truepeak");
    pMatch    = apvts.getRawParameterValue ("match");
    setLicenceInfo (licence.check());
}

juce::AudioProcessorValueTreeState::ParameterLayout AutoClipProAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto id = [] (const char* s) { return ParameterID { s, 1 }; };
    auto dbText = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1); };

    l.add (std::make_unique<AudioParameterFloat> (id ("amount"), "Amount", NormalisableRange<float> (0.0f, amountMaxDb, 0.1f), amountDefaultDb,
               AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    l.add (std::make_unique<AudioParameterChoice> (id ("type"), "Type", StringArray { "Soft", "Hard", "Tape", "Tube" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (id ("ceiling"), "Ceiling", NormalisableRange<float> (ceilingMinDb, 0.0f, 0.1f), gate::liteCeilingDb,
               AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return String (v, 1); })));
    l.add (std::make_unique<AudioParameterChoice> (id ("protect"), "Low Protect", StringArray { "Off", "60 Hz", "90 Hz", "120 Hz", "150 Hz", "Auto" }, protectAuto));
    l.add (std::make_unique<AudioParameterChoice> (id ("quality"), "Oversampling", StringArray { "Off", "2x", "4x", "8x" }, gate::liteOsIndex));
    l.add (std::make_unique<AudioParameterFloat> (id ("mix"), "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
               AudioParameterFloatAttributes().withLabel ("%")));
    l.add (std::make_unique<AudioParameterBool> (id ("truepeak"), "True Peak", false));
    l.add (std::make_unique<AudioParameterBool> (id ("match"), "Match Level", false));
    return l;
}

// ── Resolve: Lite/Pro gate -> the settings that actually run ─────────────────
AutoClipProAudioProcessor::Effective AutoClipProAudioProcessor::resolve (bool pro) const
{
    Effective e;
    e.type      = gate::effectiveType (juce::jlimit (0, numTypes - 1, (int) pType->load()), pro);
    e.osIndex   = (pro || ! gate::proQuality) ? juce::jlimit (0, numOsSteps - 1, (int) pQuality->load()) : gate::liteOsIndex;
    e.ceilingDb = (pro || ! gate::proCeiling) ? juce::jlimit (ceilingMinDb, 0.0f, pCeiling->load()) : gate::liteCeilingDb;
    e.protect   = (pro || ! gate::proProtect) ? juce::jlimit (0, numProtectChoices - 1, (int) pProtect->load()) : 0;
    e.mix       = (pro || ! gate::proMix) ? juce::jlimit (0.0f, 1.0f, pMix->load() / 100.0f) : 1.0f;
    e.truePeak  = (pro || ! gate::proTruePeak) ? pTruePeak->load() > 0.5f : false;
    e.amountDb  = juce::jlimit (0.0f, amountMaxDb, pAmount->load());
    e.match     = pMatch->load() > 0.5f;
    return e;
}

// ── Oversampling chains ──────────────────────────────────────────────────────
void AutoClipProAudioProcessor::Path::prepare (double rate, int exp, int maxBlock)
{
    exponent = exp; factor = 1 << exp;
    if (exp > 0)
    {
        // Linear-phase FIR halfband filters with whole-sample latency: the dry path (MIX) lines up exactly.
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) exp, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        os->initProcessing ((size_t) maxBlock);
        lat = (int) std::lround (os->getLatencyInSamples());
    }
    else { os.reset(); lat = 0; }
    osRate = rate * factor; autoHz = 0.0f;
    for (int k = 1; k < numProtectSteps; ++k)
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 2; ++s)
            {
                hp[k][c][s].setHighPass (osRate, protectStepsHz[k], 0.7071067811865476);
                lp[k][c][s].setLowPass  (osRate, protectStepsHz[k], 0.7071067811865476);
            }
    envRel = (float) std::exp (-1.0 / (0.010 * osRate));
    for (auto& d : dc) d.prepare (osRate);
}

void AutoClipProAudioProcessor::Path::setAutoHz (float hz)
{
    if (std::abs (hz - autoHz) < 1.0e-4f) return;
    autoHz = hz;
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 2; ++s)
        {
            hp[protectAuto][c][s].setHighPass (osRate, hz, 0.7071067811865476);
            lp[protectAuto][c][s].setLowPass  (osRate, hz, 0.7071067811865476);
        }
}

void AutoClipProAudioProcessor::Path::reset()
{
    if (os) os->reset();
    for (auto& a : hp) for (auto& b : a) for (auto& f : b) f.reset();
    for (auto& a : lp) for (auto& b : a) for (auto& f : b) f.reset();
    for (auto& a : env) for (auto& v : a) v = 0.0f;
    for (auto& d : dc) d.reset();
    for (auto& d : pad) d.reset();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void AutoClipProAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    maxPathLat = 0;
    for (int k = 0; k < numOsSteps; ++k)
    {
        paths[(size_t) k].prepare (sr, k, chunk);
        maxPathLat = juce::jmax (maxPathLat, paths[(size_t) k].lat);
    }
    for (auto& p : paths) for (auto& d : p.pad) d.prepare (maxPathLat - p.lat);

    bufA.setSize (2, chunk); bufB.setSize (2, chunk); bufD.setSize (2, chunk);
    cArr.assign ((size_t) chunk, 1.0f);
    fadeArr.assign ((size_t) chunk, 1.0f);
    for (auto& d : dryDelay) d.prepare (maxPathLat);
    cDelay.prepare (maxPathLat);
    limiter.prepare (sr);
    setLatencySamples (maxPathLat + limiter.latency());

    for (auto& k : kIn) k.prepare (sr);
    for (auto& k : kOut) k.prepare (sr);
    pCoef     = 1.0 - std::exp (-1.0 / (2.0 * sr));
    matchCoef = (float) (1.0 - std::exp (-1.0 / (0.1 * sr)));
    stAttack  = (float) (1.0 - std::exp (-1.0 / (0.002 * sr)));   // gain staging: down fast (a louder peak arrived)
    stRelease = (float) (1.0 - std::exp (-1.0 / (0.05 * sr)));    // up gently...
    stUpMax   = (float) (stagingRiseDbPerSec / sr);                // ...and never faster than 12 dB/s
    trustSamples = (juce::int64) (trustSecs * sr);
    bassFinder.prepare (sr);
    osFadeStep  = (float) (1.0 / (0.03 * sr));
    listenLen   = juce::jmax (1, (int) (0.1 * sr));
    learnLen    = juce::jmax ((juce::int64) 1, (juce::int64) (learnSecs * sr));
    learnHoldLen = juce::jmax (1, (int) (0.25 * sr));
    lEnvRel = (float) std::exp (-1.0 / (0.010 * sr)); lHz = 0.0f;
    cfgFadeStep = (float) (1.0 / (0.03 * sr));
    scopeHop = juce::jmax (1, (int) (0.005 * sr));
    sAmount.reset (sr, 0.05); sCeilDb.reset (sr, 0.1); sMix.reset (sr, 0.08);
    reset();
}

void AutoClipProAudioProcessor::reset()
{
    // Settle everything on the current values: no ramps, no fades on load.
    const auto e = resolve (proUnlocked.load());
    for (auto& p : paths) p.reset();
    curPath = e.osIndex; nextPath = -1; osFade = 1.0f; warmLeft = 0;
    cfgCur = cfgPrev = { e.type, e.protect }; cfgFade = 1.0f;
    for (auto& d : dryDelay) d.reset();
    cDelay.prepare (maxPathLat);
    { const float c0 = dbToGain (e.ceilingDb); for (int i = 0; i < maxPathLat; ++i) cDelay.process (c0); limiter.reset (c0); }
    for (auto& k : kIn) k.reset();
    for (auto& k : kOut) k.reset();
    pIn = pOut = 0.0; matchGain = 1.0f; settled = false; listenLeft = 0; learnLeft = 0;
    distE = distS = 0.0;
    sAmount.setCurrentAndTargetValue (e.amountDb);
    sCeilDb.setCurrentAndTargetValue (e.ceilingDb);
    sMix.setCurrentAndTargetValue (e.mix);
    if (refVersion.load() != localRefVersion)
    {
        localRefVersion = refVersion.load();
        applyRestore();
    }
    stagingDb = heard ? juce::jlimit (-24.0f, 24.0f, e.ceilingDb - gainToDb (refLin)) : 0.0f;
    stagingOut.store (stagingDb);
    autoHzCur = autoHzTarget;
    for (auto& p : paths) p.setAutoHz (autoHzCur);
    autoHzOut.store (autoHzCur);
    scopeCount = 0; scopePre = scopeOut = 0.0f;
}

bool AutoClipProAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

void AutoClipProAudioProcessor::requestLearn() { learnRequest.store (true); }

// LEARN has listened: stage the loudest peak at the ceiling and work out the clean AMOUNT for this material.
// The clipping error for a drive of D dB over the loudest peak is predicted from the level histogram
// (hard-clip error energy vs signal energy); LEARN takes the most drive that keeps it under learnCleanDb,
// capped at learnMaxDb. Dense material (a hat loop, a busy drum bus) gets less, sparse peaks get more.
// On the real stems in Test/Main.cpp (learnStemTests) this lands at 0.5-3 dB of AMOUNT and a -35 to -43 dB residual.
void AutoClipProAudioProcessor::finishLearn()
{
    learnLeft = 0;
    heard = learnPeak > dbToGain (-50.0f);
    if (! heard) return;
    refLin = learnPeak;
    heardSamples = trustSamples;                  // LEARN heard the track: no quiet-intro cap
    const float peakDb = gainToDb (learnPeak);
    const double sig = learnSumSq / ((double) learnPeak * learnPeak);
    float chosen = 0.0f;
    for (int step = (int) std::lround (learnMaxDb * 10.0f); step >= 0; --step)
    {
        const float d = 0.1f * (float) step;
        double err = 0.0;
        const int b0 = juce::jlimit (0, histBins - 1, (int) std::floor ((peakDb - d - histLoDb) * 10.0f));
        for (int b = b0; b < histBins; ++b)
        {
            if (learnHist[(size_t) b] == 0) continue;
            const double lvl = (double) histLoDb + (b + 0.5) * 0.1 - peakDb + d;   // dB vs the clip threshold
            if (lvl <= 0.0) continue;
            const double over = std::pow (10.0, lvl / 20.0) - 1.0;
            err += learnHist[(size_t) b] * over * over;
        }
        const double ratio = err / (sig * std::pow (10.0, d / 10.0) + 1.0e-30);
        if (ratio <= std::pow (10.0, learnCleanDb / 10.0)) { chosen = d; break; }
    }
    learnedAmountOut.store (chosen);
    learnVersion.fetch_add (1);
    triggerAsyncUpdate();                          // the dial moves on the message thread
}

bool AutoClipProAudioProcessor::applyLearnedAmount()
{
    const int v = learnVersion.load();
    if (v == appliedLearnVersion) return false;
    appliedLearnVersion = v;
    if (auto* prm = apvts.getParameter ("amount"))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (learnedAmountOut.load()));
        prm->endChangeGesture();
    }
    return true;
}

// A restored session: same learned peak (full gain staging straight away) and the same AUTO crossover.
void AutoClipProAudioProcessor::applyRestore()
{
    heard = refRestoreHeard.load(); refLin = dbToGain (refRestoreDb.load());
    if (heard) heardSamples = trustSamples;
    restoredAutoHz = autoRestoreHz.load();
    bassFinder.reset();
    if (restoredAutoHz > 0.0f) autoHzTarget = autoHzCur = restoredAutoHz;
}

// ── Clipping (oversampled rate) ──────────────────────────────────────────────
inline float AutoClipProAudioProcessor::clipOne (Path& P, int ch, float x, Config cfg, float c, const float* lows) const
{
    if (cfg.protect == 0) return c * shape (cfg.type, x / c);
    // LOW PROTECT: only the band above the crossover (LR4 high-pass) is clipped, and only to whatever room
    // the rest of the signal leaves under the ceiling, so the kick and bass waveform go through untouched.
    // Only the change the clipper makes is added back (y = x + clipped high - high): nothing is phase-shifted
    // when nothing is clipped. A last stage with a late knee (linear up to 90 % of the ceiling, so a bass
    // under that is never touched) catches the rare peak where the lows alone reach the ceiling.
    // Protection only matters when there's real low end near the top: with the lows under -20 dB of the
    // ceiling the bass can't be what gets clipped, and the high band alone (phase-shifted by the crossover)
    // peaks up to 1 dB higher than the whole signal, so clipping it would cost more than it saves. AUTO
    // therefore blends from plain clipping (lows under -20 dB) to full protection (lows over -12 dB).
    const float yFull = c * shape (cfg.type, x / c);
    const float a = protectBlend (P.env[cfg.protect][ch] / c);
    if (a <= 0.0f) return yFull;
    const float rest = lows[cfg.protect], high = x - rest;
    const float th = juce::jmax (c - P.env[cfg.protect][ch], 0.25f * c);
    const float y = rest + th * shape (cfg.type, high / th);
    const float yProt = c * std::copysign (kneeClip (std::abs (y) / c, 0.9f), y);
    return yFull + (yProt - yFull) * a;
}

void AutoClipProAudioProcessor::runPath (Path& P, float* const* io, int n, Config cur, Config prev)
{
    juce::dsp::AudioBlock<float> base (io, 2, (size_t) n);
    auto up = P.factor > 1 ? P.os->processSamplesUp (base) : base;
    const int N = (int) up.getNumSamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = up.getChannelPointer ((size_t) ch);
        auto& dc = P.dc[ch];
        for (int j = 0; j < N; ++j)
        {
            const int i = j >> P.exponent;
            const float c = cArr[(size_t) i], f = fadeArr[(size_t) i], x = d[j];
            float lows[numProtectChoices]; lows[0] = 0.0f;
            for (int k = 1; k < numProtectChoices; ++k)
            {
                lows[k] = x - P.hp[k][ch][1].process (P.hp[k][ch][0].process (x));   // everything but the high band
                // The room the lows take is measured on the real low band. (x minus the high band isn't the lows:
                // around the crossover it holds most of the low mids too, phase-shifted, which used to squeeze the
                // room for the high band down to the 25 % floor on any piano, synth or vocal: constant crunch.)
                const float low = P.lp[k][ch][1].process (P.lp[k][ch][0].process (x));
                P.env[k][ch] = juce::jmax (std::abs (low), P.env[k][ch] * P.envRel);
            }
            float y = clipOne (P, ch, x, cur, c, lows);
            if (f < 1.0f)
            {
                const float yp = clipOne (P, ch, x, prev, c, lows);
                y = yp + (y - yp) * f;
            }
            // Remove DC from what the clipper changed only (TUBE is asymmetric); the clean signal keeps its phase.
            P.errE += (double) (y - x) * (y - x); P.sigE += (double) x * x;
            d[j] = x + dc.process (y - x);
        }
    }
    if (P.factor > 1) P.os->processSamplesDown (base);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i) io[ch][i] = P.pad[ch].process (io[ch][i]);
}

// ── Audio ─────────────────────────────────────────────────────────────────────
void AutoClipProAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (2, getTotalNumInputChannels(), getTotalNumOutputChannels(), buffer.getNumChannels());
    for (int c = juce::jmax (0, numCh); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (numCh <= 0 || n <= 0) return;
    for (int pos = 0; pos < n; pos += chunk)
    {
        float* io[2] = { buffer.getWritePointer (0, pos), buffer.getWritePointer (numCh > 1 ? 1 : 0, pos) };
        processChunk (io, numCh, juce::jmin (chunk, n - pos));
    }
}

void AutoClipProAudioProcessor::processChunk (float* const* io, int numCh, int n)
{
    // Restored session / LEARN
    if (refVersion.load() != localRefVersion)
    {
        localRefVersion = refVersion.load();
        applyRestore();
        if (heard) stagingDb = juce::jlimit (-24.0f, 24.0f, sCeilDb.getCurrentValue() - gainToDb (refLin));
    }
    // LEARN: listen to learnSecs of signal (silence doesn't count), then stage the loudest peak at the ceiling
    // and set a clean AMOUNT (finishLearn). The gain staging holds still meanwhile (no dip, no jump; a louder
    // peak still turns it down), then glides to the new value at 12 dB/s at most.
    // LEARN also means "this is my material": once it has listened, the boost cap for a fresh instance is
    // lifted, and LOW PROTECT AUTO listens for the bass again.
    if (learnRequest.exchange (false))
    {
        learnLeft = learnLen; learnHold = 0; lHz = 0.0f; learnPeak = 0.0f; learnSumSq = 0.0; learnHist.fill (0);
        listenLeft = 0; bassFinder.reset(); restoredAutoHz = 0.0f;
    }

    // Locked features fall back to their Lite values, so automation or an old session can't bypass the lock.
    const auto e = resolve (proUnlocked.load());
    sAmount.setTargetValue (e.amountDb); sCeilDb.setTargetValue (e.ceilingDb); sMix.setTargetValue (e.mix);
    const Config want { e.type, e.protect };
    if (want != cfgCur) { cfgPrev = cfgCur; cfgCur = want; cfgFade = 0.0f; }

    // QUALITY change: warm the new chain up silently, then crossfade 30 ms (no clicks, same latency).
    auto startPath = [this] (int k) { nextPath = k; paths[(size_t) k].reset(); warmLeft = maxPathLat + 64; osFade = 0.0f; };
    if (nextPath < 0) { if (e.osIndex != curPath) startPath (e.osIndex); }
    else if (e.osIndex != nextPath)
    {
        if (e.osIndex == curPath) { nextPath = -1; osFade = 1.0f; }
        else startPath (e.osIndex);
    }

    float* A[2] = { bufA.getWritePointer (0), bufA.getWritePointer (1) };
    float* B[2] = { bufB.getWritePointer (0), bufB.getWritePointer (1) };
    float* D[2] = { bufD.getWritePointer (0), bufD.getWritePointer (1) };   // pre-clip signal, delayed to line up with the clipper

    const bool twoPaths = nextPath >= 0;
    const float heardGate = dbToGain (-50.0f);
    // LEARN's view of LOW PROTECT (same crossover as the clipper, at the base rate)
    const float protHz = e.protect == 0 ? 0.0f : e.protect == protectAuto ? autoHzCur : protectStepsHz[e.protect];
    if (learnLeft > 0 && protHz > 0.0f && std::abs (protHz - lHz) > 0.01f)
    {
        if (lHz <= 0.0f) { for (auto& a : lHp) for (auto& f : a) f.reset(); for (auto& a : lLp) for (auto& f : a) f.reset(); lEnv[0] = lEnv[1] = 0.0f; }
        lHz = protHz;
        for (int ch = 0; ch < 2; ++ch)
            for (int st = 0; st < 2; ++st) { lHp[ch][st].setHighPass (sr, lHz, 0.7071067811865476); lLp[ch][st].setLowPass (sr, lHz, 0.7071067811865476); }
    }
    if (protHz <= 0.0f) lHz = 0.0f;
    float blkIn = 0.0f;

    // Pass 1 (base rate): gain staging, ceiling, dry delay, input loudness.
    for (int i = 0; i < n; ++i)
    {
        const float x0 = io[0][i], x1 = numCh > 1 ? io[1][i] : x0;
        const float pk = juce::jmax (std::abs (x0), std::abs (x1));
        blkIn = juce::jmax (blkIn, pk);
        bool newPeak = false;
        if (learnLeft > 0)
        {
            if (heard && pk > refLin) { refLin = pk; newPeak = true; }   // a louder peak still turns the gain down right away
            // Only time with music counts: from a sound over the gate until 0.25 s after the last one
            // (so the quiet between hits counts, a stopped transport or a silent intro doesn't).
            if (pk > heardGate) learnHold = learnHoldLen;
            if (learnHold > 0)
            {
                --learnHold;
                learnPeak = juce::jmax (learnPeak, pk);
                learnSumSq += (double) pk * pk;
                float lv = pk;
                if (lHz > 0.0f)
                {
                    lv = 0.0f;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float xc = ch == 0 ? x0 : x1;
                        const float hi = lHp[ch][1].process (lHp[ch][0].process (xc));
                        const float lo = lLp[ch][1].process (lLp[ch][0].process (xc));
                        lEnv[ch] = juce::jmax (std::abs (lo), lEnv[ch] * lEnvRel);
                        const float a = protectBlend (lEnv[ch] / learnPeak);   // vs the loudest peak so far (= the ceiling once staged)
                        lv = juce::jmax (lv, std::abs (xc) + (std::abs (hi) + lEnv[ch] - std::abs (xc)) * a);
                    }
                }
                if (lv > 1.0e-5f) ++learnHist[(size_t) juce::jlimit (0, histBins - 1, (int) ((gainToDb (lv) - histLoDb) * 10.0f))];
                if (--learnLeft == 0) finishLearn();
            }
        }
        else if (listenLeft > 0)
        {
            candidate = juce::jmax (candidate, pk);
            if (heard && pk > refLin) { refLin = pk; newPeak = true; }   // a louder peak still turns the gain down right away
            if (--listenLeft == 0)
            {
                heard = candidate > heardGate;
                refLin = heard ? candidate : 1.0f;
            }
        }
        else if (! heard) { if (pk > heardGate) { listenLeft = listenLen; candidate = pk; } }   // first sound: listen 100 ms
        else if (pk > refLin) { refLin = pk; newPeak = true; }

        const float ceilDb = sCeilDb.getNextValue();
        if (heard && heardSamples < trustSamples && pk > heardGate) ++heardSamples;
        const float cap = heardSamples >= trustSamples ? 24.0f
                        : capStartDb + (24.0f - capStartDb) * (float) heardSamples / (float) trustSamples;
        const float target = heard ? juce::jlimit (-24.0f, cap, ceilDb - gainToDb (refLin)) : 0.0f;
        // A louder peak than any heard so far: down at once, on that sample, so it meets the clipper at the
        // same AMOUNT as every other peak (a 2 ms glide let the first hit of a louder drop slam in up to
        // 20 dB hot). Any other drop (LEARN, the ceiling) glides.
        if (newPeak && target < stagingDb) stagingDb = target;
        else if (target < stagingDb) stagingDb += (target - stagingDb) * stAttack;
        else                    stagingDb += juce::jmin ((target - stagingDb) * stRelease, stUpMax);
        bassFinder.push (0.5f * (x0 + x1));
        const float g = dbToGain (stagingDb + sAmount.getNextValue());
        const float p0 = x0 * g, p1 = x1 * g;
        A[0][i] = p0; A[1][i] = p1;
        if (twoPaths) { B[0][i] = p0; B[1][i] = p1; }
        D[0][i] = dryDelay[0].process (p0);
        D[1][i] = dryDelay[1].process (p1);
        cArr[(size_t) i] = dbToGain (ceilDb);
        fadeArr[(size_t) i] = cfgFade;
        if (cfgFade < 1.0f) cfgFade = juce::jmin (1.0f, cfgFade + cfgFadeStep);

        const float k0 = kIn[0].process (x0), k1 = kIn[1].process (x1);
        pIn += (0.5 * ((double) k0 * k0 + (double) k1 * k1) - pIn) * pCoef;
    }

    // LOW PROTECT AUTO: follow what the bass finder has heard (a restored session keeps its crossover unless
    // a higher bass turns up), gliding so the crossover never jumps.
    {
        const float est = bassFinder.crossoverHz();
        if (est > 0.0f) autoHzTarget = juce::jmax (est, restoredAutoHz);
        else if (restoredAutoHz > 0.0f) autoHzTarget = restoredAutoHz;
        if (std::abs (autoHzCur - autoHzTarget) > 1.0e-4f)
        {
            const float a = 1.0f - std::exp (-(float) n / (float) (0.3 * sr));
            autoHzCur = std::abs (autoHzTarget - autoHzCur) < 0.05f ? autoHzTarget : std::exp (std::log (autoHzCur) + (std::log (autoHzTarget) - std::log (autoHzCur)) * a);
            paths[(size_t) curPath].setAutoHz (autoHzCur);
            if (twoPaths) paths[(size_t) nextPath].setAutoHz (autoHzCur);
        }
    }

    // Pass 2 (oversampled): the clipper.
    runPath (paths[(size_t) curPath], A, n, cfgCur, cfgPrev);
    if (twoPaths) runPath (paths[(size_t) nextPath], B, n, cfgCur, cfgPrev);

    // Pass 3 (base rate): MIX, safety limiter, MATCH, meters.
    const bool tp = e.truePeak;
    float blkOut = 0.0f, blkClip = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        float w0 = A[0][i], w1 = A[1][i];
        if (twoPaths)
        {
            if (warmLeft > 0) --warmLeft;
            else
            {
                osFade = juce::jmin (1.0f, osFade + osFadeStep);
                w0 += (B[0][i] - w0) * osFade; w1 += (B[1][i] - w1) * osFade;
            }
        }
        const float d0 = D[0][i], d1 = D[1][i];
        const float c = cDelay.process (cArr[(size_t) i]);   // the ceiling the clipper used for these samples
        const float mix = sMix.getNextValue();
        float m0 = d0 + (w0 - d0) * mix, m1 = d1 + (w1 - d1) * mix;
        limiter.process (m0, m1, c, tp);

        const float k0 = kOut[0].process (m0), k1 = kOut[1].process (m1);
        pOut += (0.5 * ((double) k0 * k0 + (double) k1 * k1) - pOut) * pCoef;
        const float mt = (e.match && heard && pOut > 1.0e-12) ? (float) juce::jmin (1.0, std::sqrt (pIn / pOut)) : 1.0f;
        matchGain += (mt - matchGain) * matchCoef;

        const float preAbs = juce::jmax (std::abs (d0), std::abs (d1));
        const float outAbs = juce::jmax (std::abs (m0), std::abs (m1));
        blkClip = juce::jmax (blkClip, preAbs / c);
        scopePre = juce::jmax (scopePre, preAbs / c);
        scopeOut = juce::jmax (scopeOut, outAbs / c);
        if (++scopeCount >= scopeHop)
        {
            const int w = scopeWrite.load();
            scope[(size_t) w].pre.store (scopePre);
            scope[(size_t) w].out.store (scopeOut);
            scopeWrite.store ((w + 1) % scopeSize);
            scopeCount = 0; scopePre = scopeOut = 0.0f;
        }

        m0 *= matchGain; m1 *= matchGain;
        blkOut = juce::jmax (blkOut, juce::jmax (std::abs (m0), std::abs (m1)));
        io[0][i] = m0;
        if (numCh > 1) io[1][i] = m1;
    }
    if (twoPaths && osFade >= 1.0f) { curPath = nextPath; nextPath = -1; }

    // Distortion meter: clipper change vs signal, ~0.5 s.
    {
        auto& P = paths[(size_t) curPath];
        const double k = std::exp (-(double) n / (0.5 * sr));
        distE = distE * k + P.errE; distS = distS * k + P.sigE;
        for (auto& q : paths) q.errE = q.sigE = 0.0;   // (the other chain, if QUALITY was crossfading)
        clipDistDb.store (distS > 1.0e-9 ? (float) (10.0 * std::log10 (distE / distS + 1.0e-12)) : -120.0f);
    }

    inPeak.store (blkIn); outPeak.store (blkOut); clipDb.store (gainToDb (blkClip));
    { float h = clipHoldDb.load(); const float v = gainToDb (blkClip); while (v > h && ! clipHoldDb.compare_exchange_weak (h, v)) {} }
    louderDb.store (pIn > 1.0e-12 && pOut > 1.0e-12 ? (float) (10.0 * std::log10 (pOut / pIn)) : 0.0f);
    matchDb.store (gainToDb (matchGain));
    ceilingNow.store (cArr[(size_t) (n - 1)]);
    refDbOut.store (heard ? gainToDb (refLin) : -120.0f);
    stagingOut.store (stagingDb);
    autoHzOut.store (autoHzCur);
    heardOut.store (heard);
    listeningOut.store (listenLeft > 0 || learnLeft > 0);
    learnProgOut.store (learnLeft > 0 ? 1.0f - (float) learnLeft / (float) learnLen : 0.0f);
}

// ── State ─────────────────────────────────────────────────────────────────────
void AutoClipProAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("Ref"), nullptr);
    juce::ValueTree ref ("Ref");
    ref.setProperty ("heard", heardOut.load(), nullptr);
    ref.setProperty ("peakDb", (double) refDbOut.load(), nullptr);
    ref.setProperty ("autoHz", (double) autoHzOut.load(), nullptr);
    state.appendChild (ref, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void AutoClipProAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (! xml || ! xml->hasTagName (apvts.state.getType())) return;
    auto tree = juce::ValueTree::fromXml (*xml);
    const auto ref = tree.getChildWithName ("Ref");
    const bool h = ref.isValid() && (bool) ref.getProperty ("heard", false);
    const float pk = ref.isValid() ? (float) (double) ref.getProperty ("peakDb", -120.0) : -120.0f;
    const float ahz = ref.isValid() ? (float) (double) ref.getProperty ("autoHz", 0.0) : 0.0f;
    tree.removeChild (ref, nullptr);
    apvts.replaceState (tree);

    // A saved session keeps its learned peak: same gain staging, no re-learn.
    refRestoreHeard.store (h && pk > -100.0f);
    refRestoreDb.store (juce::jlimit (-100.0f, 24.0f, pk));
    heardOut.store (h && pk > -100.0f);
    refDbOut.store (h ? pk : -120.0f);
    const float a = ahz >= autoMinHz && ahz <= autoMaxHz ? ahz : 0.0f;
    autoRestoreHz.store (a);
    if (a > 0.0f) autoHzOut.store (a);
    refVersion.fetch_add (1);
}

juce::AudioProcessorEditor* AutoClipProAudioProcessor::createEditor() { return new AutoClipProAudioProcessorEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AutoClipProAudioProcessor(); }
