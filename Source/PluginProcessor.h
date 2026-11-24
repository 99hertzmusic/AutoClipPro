#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include "Features.h"
#include "ClipEngine.h"
#include "Licence/Licence.h"

// AutoClip Pro: the automatic clipper.
//  Lite (free): AMOUNT, automatic gain staging (learns the track's loudest peak; LEARN listens 6 s and sets the
//  level and a clean AMOUNT for the track),
//  SOFT clip at a fixed -0.3 dB ceiling with 4x oversampling, MATCH (hear it at the same level), the clip display.
//  Pro (key or 7-day trial): HARD / TAPE / TUBE, CEILING, LOW PROTECT, QUALITY (oversampling), MIX, TRUE PEAK.
//  The split itself lives in Features.h (ac::gate).
class AutoClipProAudioProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    AutoClipProAudioProcessor();
    ~AutoClipProAudioProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "AutoClip Pro"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ── Licence ─────────────────────────────────────────────────────────────
    hz::Licence licence { "autoclippro" };
    hz::LicenceInfo licenceInfo;                 // message thread
    std::atomic<bool> proUnlocked { false };     // audio thread reads this
    std::atomic<int> licenceVersion { 0 };       // bumps on every change, so open editors can follow
    void setLicenceInfo (const hz::LicenceInfo& i) { licenceInfo = i; proUnlocked.store (i.unlocked()); ++licenceVersion; }
    std::unique_ptr<hz::LicenceWatcher> licenceWatcher;   // picks up keys saved by other instances / trial expiry
    // True if the current settings include upgrade-only values (kept in the session, played as free).
    bool usesUpgradeSettings() const;

    // ── What actually runs, after the Lite/Pro gate ────────────────────────
    struct Effective
    {
        int   type = ac::soft;
        int   osIndex = ac::gate::liteOsIndex;
        float ceilingDb = ac::gate::liteCeilingDb;
        int   protect = 0;
        float mix = 1.0f;
        bool  truePeak = false;
        float amountDb = ac::amountDefaultDb;
        bool  match = false;
    };
    Effective resolve (bool pro) const;

    // ── Auto gain staging (the learned loudest input peak) ──────────────────
    // LEARN key: forget the peak, listen to learnSecs of signal, then set the gain staging AND put the clean
    // AMOUNT for this material on the dial (the most clipping that stays clean, never more than learnMaxDb).
    void requestLearn();
    bool  hasHeardSignal() const { return heardOut.load(); }
    bool  isListening() const    { return listeningOut.load(); }
    float getLearnProgress() const { return learnProgOut.load(); }    // 0..1 while LEARN listens
    float getLearnedAmountDb() const { return learnedAmountOut.load(); }
    int   getLearnCount() const { return learnVersion.load(); }       // LEARNs finished so far
    bool  applyLearnedAmount();                  // message thread: puts the AMOUNT LEARN found on the dial (once per LEARN)
    static constexpr double learnSecs = 6.0;     // signal LEARN listens to (silence doesn't count)
    static constexpr float  learnMaxDb = 3.0f;   // LEARN never asks for more clipping than this on the loudest peak
    static constexpr float  learnCleanDb = -40.0f;   // ...and keeps the clipping error this far under the signal
    float getInputPeakDb() const { return refDbOut.load(); }
    float getAutoProtectHz() const { return autoHzOut.load(); }   // LOW PROTECT AUTO: the crossover running now
    float getStagingDb() const   { return stagingOut.load(); }    // gain staging applied now (before AMOUNT)

    // ── Meters / display for the editor ────────────────────────────────────
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, clipDb { 0.0f };
    // How much the clipper changes the signal (error energy vs signal, dB, ~0.5 s): what you'd hear as distortion.
    std::atomic<float> clipDistDb { -120.0f };
    std::atomic<float> clipHoldDb { 0.0f };      // highest CLIP since the editor last read it (exchange it to 0)
    // Meter colours: clean under -30 dB (no dirtier than Pro-L 2 at +2.5 dB on a drum loop), audible to -20 dB, then hot.
    static constexpr float distCleanDb = -30.0f, distHeavyDb = -20.0f;
    std::atomic<float> louderDb { 0.0f };        // output vs input loudness (before MATCH), K-weighted, ~3 s
    std::atomic<float> matchDb { 0.0f };         // gain MATCH is applying
    std::atomic<float> ceilingNow { 0.0f };      // linear ceiling actually running
    static constexpr int scopeSize = 400;        // 5 ms per column = 2 s
    struct ScopeColumn { std::atomic<float> pre { 0 }, out { 0 }; };
    std::array<ScopeColumn, scopeSize> scope;
    std::atomic<int> scopeWrite { 0 };

    // ── Test hooks ─────────────────────────────────────────────────────────
    int  getActiveOsIndex() const { return curPath; }
    int  getPathLatency (int osIndex) const { return paths[(size_t) osIndex].lat; }
    int  getMaxPathLatency() const { return maxPathLat; }

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void processChunk (float* const* io, int numCh, int n);
    void applyRestore();
    void finishLearn();
    void handleAsyncUpdate() override { applyLearnedAmount(); }

    // One oversampling chain (OFF / 2x / 4x / 8x). All four are built in prepareToPlay; one runs
    // (two while QUALITY crossfades). Latency is padded to the slowest chain so it never changes.
    struct Path
    {
        int factor = 1, exponent = 0, lat = 0;
        std::unique_ptr<juce::dsp::Oversampling<float>> os;
        ac::Biquad hp[ac::numProtectChoices][2][2];  // LOW PROTECT crossovers (LR4 high-pass), [step][ch][stage]; last = AUTO
        ac::Biquad lp[ac::numProtectChoices][2][2];  // the real low band (LR4 low-pass): only measures how much room the lows take
        float env[ac::numProtectChoices][2] {};
        double osRate = 44100.0; float autoHz = 0.0f;
        void setAutoHz (float hz);
        float envRel = 0.999f;
        ac::DcBlock dc[2];
        double errE = 0.0, sigE = 0.0;               // clipper change / signal energy, this chunk (meter)
        ac::Delay pad[2];
        void prepare (double sr, int exp, int chunk);
        void reset();
    };
    struct Config { int type = 0, protect = 0; bool operator!= (const Config& o) const { return type != o.type || protect != o.protect; } };
    void runPath (Path& P, float* const* io, int n, Config cur, Config prev);
    inline float clipOne (Path& P, int ch, float x, Config cfg, float c, const float* lows) const;

    std::atomic<float>* pAmount = nullptr; std::atomic<float>* pType = nullptr; std::atomic<float>* pCeiling = nullptr;
    std::atomic<float>* pProtect = nullptr; std::atomic<float>* pQuality = nullptr; std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pTruePeak = nullptr; std::atomic<float>* pMatch = nullptr;

    double sr = 44100.0;
    static constexpr int chunk = 512;
    std::array<Path, ac::numOsSteps> paths;
    int maxPathLat = 0;
    int curPath = ac::gate::liteOsIndex, nextPath = -1, warmLeft = 0;
    float osFade = 1.0f, osFadeStep = 0.001f;

    Config cfgCur, cfgPrev;
    float cfgFade = 1.0f, cfgFadeStep = 0.001f;

    // Per-chunk scratch (allocated in prepareToPlay only)
    juce::AudioBuffer<float> bufA, bufB, bufD;   // chain A, chain B (QUALITY crossfade), delayed dry
    std::vector<float> cArr, fadeArr;

    // Gain staging. Until enough has been heard, the boost is capped (a quiet intro isn't the loudest part
    // of the track) and it only ever rises gently: no jump when the plugin first hears sound.
    float refLin = 1.0f;  bool heard = false;
    juce::int64 heardSamples = 0, trustSamples = 0;    // signal heard so far / when the boost cap is fully off
    float stUpMax = 0.0001f;                            // fastest rise, dB per sample
    int listenLeft = 0, listenLen = 4800; float candidate = 0.0f;
    float stagingDb = 0.0f, stAttack = 0.01f, stRelease = 0.001f;
    std::atomic<bool> learnRequest { false };
    std::atomic<int>  refVersion { 0 }; int localRefVersion = 0;
    std::atomic<float> refRestoreDb { 0.0f }; std::atomic<bool> refRestoreHeard { false };
    std::atomic<float> refDbOut { -120.0f }; std::atomic<bool> heardOut { false }, listeningOut { false };
    std::atomic<float> stagingOut { 0.0f };

    // LEARN: a level histogram (0.1 dB bins, dBFS) of what it heard, to work out how much clipping stays clean.
    static constexpr int histBins = 1300; static constexpr float histLoDb = -100.0f;
    std::array<int, histBins> learnHist {};
    double learnSumSq = 0.0;
    // With LOW PROTECT on, only the high band is clipped, into the room the lows leave: what LEARN histograms
    // is then |high band| + the held low-band peak (the level that actually meets the clipper).
    ac::Biquad lHp[2][2], lLp[2][2]; float lEnv[2] {}, lEnvRel = 0.999f, lHz = 0.0f;
    juce::int64 learnLeft = 0, learnLen = 1; int learnHold = 0, learnHoldLen = 12000;
    float learnPeak = 0.0f;
    std::atomic<float> learnProgOut { 0.0f }, learnedAmountOut { 0.0f };
    std::atomic<int> learnVersion { 0 }; int appliedLearnVersion = 0;
    double distE = 0.0, distS = 0.0;

    // LOW PROTECT AUTO
    ac::BassFinder bassFinder;
    float autoHzCur = ac::autoDefaultHz, autoHzTarget = ac::autoDefaultHz, restoredAutoHz = 0.0f;
    std::atomic<float> autoHzOut { ac::autoDefaultHz };
    std::atomic<float> autoRestoreHz { 0.0f };
    std::atomic<bool> finderResetRequest { false };
    juce::SmoothedValue<float> sAmount, sCeilDb, sMix;
    ac::Delay dryDelay[2], cDelay;
    ac::Limiter limiter;

    // MATCH + loudness readout
    ac::KWeight kIn[2], kOut[2];
    double pIn = 0.0, pOut = 0.0, pCoef = 0.0;
    float matchGain = 1.0f, matchCoef = 0.001f;
    bool settled = false;

    int scopeCount = 0, scopeHop = 240; float scopePre = 0.0f, scopeOut = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoClipProAudioProcessor)
};
