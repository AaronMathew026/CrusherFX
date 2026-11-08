// PC-side effects: turns slider settings into an Equalizer APO config file.
// The app never touches audio samples. Equalizer APO runs these filters inside the Windows audio
// engine, so the app has no audio thread and uses no CPU while idle.
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

struct Peq { float fc, gain, q; };

// From tools/fit_tuning.py: SoundGuys B&K 5128 measurement of the Crusher 1080 ANC against an
// approximate Harman-style target. Applied above 200 Hz only, at most 5 dB per filter.
static const Peq kTuning[] = {
    {420, 1.5f, 1.4f},
    {3190, -3.0f, 2.0f},
    {7000, -5.0f, 0.7f},
    {10360, -2.5f, 2.0f},
};

static const float kEqFreqs[10] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
enum { EQ_BANDS = 10 };
static const float kEqQ = 1.41f;

struct DspSettings {
    bool enabled = false;
    bool tuning = true;
    int clarity = 0;    // 0..100
    int bass = 0;       // -6..+10 dB
    int surround = 0;   // 0..100
    int ambience = 0;   // 0..100
    int eq[EQ_BANDS] = {};  // -12..+12 dB
};

struct FxPreset { const wchar_t* name; int clarity, bass, surround, ambience; };
static const FxPreset kFxPresets[] = {
    {L"Reference", 0, 0, 0, 0},
    {L"Music", 25, 2, 15, 0},
    {L"Bass", 15, 6, 10, 0},
    {L"Game", 40, -1, 40, 10},
    {L"Movie", 20, 3, 35, 20},
    {L"Voice", 50, -4, 0, 0},
};
enum { FX_PRESET_COUNT = 6 };

inline int matchFxPreset(const DspSettings& d) {
    for (int i = 0; i < FX_PRESET_COUNT; i++) {
        const FxPreset& p = kFxPresets[i];
        if (d.clarity == p.clarity && d.bass == p.bass && d.surround == p.surround && d.ambience == p.ambience) return i;
    }
    return -1;
}

// ---- filter list ----------------------------------------------------------------------------
enum FilterType { F_PK, F_LSC, F_HSC };
struct Filter { FilterType type; float fc, gain, q; };
enum { MAX_FILTERS = 24 };

inline int collectFilters(const DspSettings& d, Filter* out) {
    int n = 0;
    if (d.tuning)
        for (const Peq& p : kTuning) out[n++] = {F_PK, p.fc, p.gain, p.q};
    if (d.bass != 0) out[n++] = {F_LSC, 85, (float)d.bass, 0.7f};
    if (d.clarity > 0) {
        // Clarity is EQ only: less mud, more presence and air. EQ can't add harmonics the way
        // FxSound's exciter does, and Equalizer APO has no exciter built in.
        float c = d.clarity / 100.0f;
        out[n++] = {F_PK, 300, -1.5f * c, 0.8f};
        out[n++] = {F_PK, 1800, 1.5f * c, 1.0f};
        out[n++] = {F_HSC, 6000, 4.0f * c, 0.7f};
    }
    for (int i = 0; i < EQ_BANDS; i++)
        if (d.eq[i] != 0) out[n++] = {F_PK, kEqFreqs[i], (float)d.eq[i], kEqQ};
    return n;
}

// Magnitude in dB of one RBJ biquad at frequency f.
inline double filterDb(const Filter& fl, double f, double fs = 48000.0) {
    const double pi = 3.14159265358979323846;
    double A = pow(10.0, fl.gain / 40.0);
    double w0 = 2 * pi * fl.fc / fs;
    double cw = cos(w0), alpha = sin(w0) / (2 * fl.q);
    double b0, b1, b2, a0, a1, a2;
    if (fl.type == F_PK) {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        double s = 2 * sqrt(A) * alpha;
        if (fl.type == F_LSC) {
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
        }
    }
    double w = 2 * pi * f / fs;
    double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

inline double curveDb(const Filter* fl, int n, double f) {
    double db = 0;
    for (int i = 0; i < n; i++) db += filterDb(fl[i], f);
    return db;
}

inline double surroundAmount(const DspSettings& d) { return d.surround / 100.0; }
inline double ambienceAmount(const DspSettings& d) { return d.ambience / 400.0; }  // at most 0.25

// Equalizer APO has no limiter, so clipping is prevented by leaving enough headroom up front:
// the highest EQ peak plus the worst-case sum of the stereo mixers.
inline double requiredPreamp(const DspSettings& d) {
    Filter fl[MAX_FILTERS];
    int n = collectFilters(d, fl);
    double peak = 0;
    for (double f = 20; f <= 20000; f *= 1.03) {
        double v = curveDb(fl, n, f);
        if (v > peak) peak = v;
    }
    double mix = (1.0 + surroundAmount(d)) * (1.0 + ambienceAmount(d));
    return -(peak + 20.0 * log10(mix));
}

// ---- config text ----------------------------------------------------------------------------
inline std::string buildConfig(const DspSettings& d) {
    std::string out = "# Written by CrusherFX. Edits here are overwritten; use config.txt for your own filters.\n";
    if (!d.enabled) return out + "# PC effects are off.\n";
    char line[160];
    out += "Device: CRUSHER 1080\nChannel: all\n";

    double pre = requiredPreamp(d);
    if (pre < -0.05) {
        snprintf(line, sizeof line, "Preamp: %.1f dB\n", floor(pre * 10.0) / 10.0);
        out += line;
    }

    Filter fl[MAX_FILTERS];
    int n = collectFilters(d, fl);
    for (int i = 0; i < n; i++) {
        const Filter& f = fl[i];
        const char* t = f.type == F_PK ? "PK" : f.type == F_LSC ? "LSC" : "HSC";
        snprintf(line, sizeof line, "Filter %d: ON %s Fc %.0f Hz Gain %.2f dB Q %.2f\n", i + 1, t, f.fc, f.gain, f.q);
        out += line;
    }

    double k = surroundAmount(d);
    if (k > 0) {
        // Mid/side widening that keeps the centre level: L' = (1+k/2)L - (k/2)R.
        snprintf(line, sizeof line, "Copy: L=%.3f*L+-%.3f*R R=%.3f*R+-%.3f*L\n", 1 + k / 2, k / 2, 1 + k / 2, k / 2);
        out += line;
    }

    double g = ambienceAmount(d);
    if (g > 0) {
        // Delayed, band-limited cross-feed through two virtual channels. They are not physical
        // outputs, so Equalizer APO discards them at the end of the chain.
        out += "Copy: XL=1.0*L XR=1.0*R\nChannel: XL\nDelay: 11 ms\nChannel: XR\nDelay: 17 ms\nChannel: XL XR\n";
        snprintf(line, sizeof line, "Filter %d: ON HP Fc 200 Hz\nFilter %d: ON LP Fc 6000 Hz\n", n + 1, n + 2);
        out += line;
        snprintf(line, sizeof line, "Channel: all\nCopy: L=L+%.3f*XR R=R+%.3f*XL\n", g, g);
        out += line;
    }
    out += "Device: all\n";
    return out;
}

// ---- config.txt patching --------------------------------------------------------------------
inline std::string trimLine(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Adds our Include line to Equalizer APO's config.txt. If the file holds nothing but the installer's
// demo settings, those are commented out too, since they would otherwise colour the sound. Any other
// content is the user's own and stays active. Returns false when no change is needed.
inline bool patchConfigTxt(const std::string& in, std::string& out) {
    static const char* kInclude = "Include: CrusherFX.txt";
    std::string lines[256];
    int count = 0, active = 0;
    bool hasInclude = false, demoPreamp = false, demoInclude = false;
    size_t pos = 0;
    while (pos <= in.size() && count < 256) {
        size_t nl = in.find('\n', pos);
        std::string l = in.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        lines[count++] = l;
        std::string t = trimLine(l);
        if (!t.empty() && t[0] != '#') {
            active++;
            if (t == kInclude) hasInclude = true;
            if (t == "Preamp: -6 dB") demoPreamp = true;
            if (t == "Include: example.txt") demoInclude = true;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (hasInclude) return false;
    bool onlyDemo = demoPreamp && demoInclude && active == 2;
    out.clear();
    for (int i = 0; i < count; i++) {
        std::string t = trimLine(lines[i]);
        bool last = i == count - 1;
        if (last && t.empty()) break;
        if (onlyDemo && (t == "Preamp: -6 dB" || t == "Include: example.txt")) out += "# Disabled by CrusherFX: " + t + "\n";
        else out += lines[i] + "\n";
    }
    out += std::string(kInclude) + "\n";
    return true;
}
