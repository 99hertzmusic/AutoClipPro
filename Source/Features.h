#pragma once
// AutoClip Pro: clip types, stepped dial values and THE Lite/Pro split.
// Everything about what is free and what needs Pro lives in this file. To move a feature,
// flip its flag below: the processor (audio), the editor (padlocks) and the tests all read it.

namespace ac
{
enum Type { soft = 0, hard, tape, tube, numTypes };

inline const char* typeName (int t)
{
    static const char* n[] = { "SOFT", "HARD", "TAPE", "TUBE" };
    return n[t >= 0 && t < numTypes ? t : 0];
}

// Stepped dials.
inline constexpr int   osFactors[]      = { 1, 2, 4, 8 };                 // QUALITY: OFF / 2X / 4X / 8X oversampling
inline constexpr float protectStepsHz[] = { 0.0f, 60.0f, 90.0f, 120.0f, 150.0f };   // LOW PROTECT crossover, 0 = OFF
inline constexpr int numOsSteps = 4, numProtectSteps = 5;
// LOW PROTECT AUTO (the last dial step, Pro default): the crossover sits an octave above the lowest strong
// bass note it hears (kick / bass fundamental), so the bass itself never reaches the clipper.
inline constexpr int   protectAuto = numProtectSteps, numProtectChoices = numProtectSteps + 1;
inline constexpr float autoMinHz = 60.0f, autoMaxHz = 150.0f, autoDefaultHz = 120.0f, autoRatio = 2.0f;

inline constexpr float amountMaxDb = 12.0f, amountDefaultDb = 1.5f;
inline constexpr float ceilingMinDb = -12.0f;

// ── THE LITE / PRO SPLIT ──────────────────────────────────────────────────────
// true = needs Pro (licence key or 7-day trial). false = free in Lite.
// Always free: AMOUNT, auto gain staging + LEARN, MATCH (level-matched A/B), the clip display and meters.
namespace gate
{
    inline constexpr bool proType[numTypes] = {
        false,  // SOFT  (clean, the AutoClip sound)
        true,   // HARD
        true,   // TAPE
        true    // TUBE
    };
    inline constexpr bool proCeiling  = true;   // Lite: fixed at liteCeilingDb
    inline constexpr bool proQuality  = true;   // Lite: fixed at liteOsIndex
    inline constexpr bool proProtect  = true;   // Lite: OFF (Pro default: AUTO)
    inline constexpr bool proMix      = true;   // Lite: 100 %
    inline constexpr bool proTruePeak = true;   // Lite: sample-peak safety only

    inline constexpr float liteCeilingDb = -0.3f;
    inline constexpr int   liteOsIndex   = 2;      // 4x

    inline bool typeAllowed (int t, bool pro) { return pro || ! proType[t >= 0 && t < numTypes ? t : 0]; }
    // What a locked type falls back to in Lite.
    inline int  effectiveType (int t, bool pro) { return typeAllowed (t, pro) ? t : (int) soft; }
}
} // namespace ac
