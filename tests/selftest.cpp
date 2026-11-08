// Offline tests. The byte strings marked REAL were captured from a Crusher 1080 ANC (firmware 1.2.0).
#include <stdio.h>
#include <stdlib.h>

#include "../src/dsp.h"
#include "../src/model.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (c) g_pass++;                                          \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static size_t hex(const char* s, uint8_t* out) {
    size_t n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        out[n++] = (uint8_t)strtol(std::string(s, 2).c_str(), nullptr, 16);
        s += 2;
    }
    return n;
}

// Wraps a real response body in a valid frame, then feeds it through the whole parser.
static void feed(HsState& st, int type, int svc, int m, const char* pbHex) {
    uint8_t pb[256], f[300];
    size_t pn = hex(pbHex, pb);
    size_t body = 4 + pn;
    f[0] = 0x7E; f[1] = 0xFF; f[2] = 0x80; f[3] = 0x94;
    f[4] = (uint8_t)(body >> 8); f[5] = (uint8_t)body;
    f[6] = bos::crc8(f, 6);
    f[7] = (uint8_t)(0x10 | (type << 2)); f[8] = (uint8_t)svc; f[9] = (uint8_t)m; f[10] = 1;
    memcpy(f + 11, pb, pn);
    uint16_t c = bos::crc16(f + 1, 6 + body);
    f[7 + body] = (uint8_t)(c >> 8); f[8 + body] = (uint8_t)c;
    bos::Packet p;
    size_t used;
    CHECK(bos::parseFrame(f, 9 + body, p, used) == bos::PARSE_FRAME);
    CHECK(used == 9 + body);
    hsApply(st, p);
}

static void testFraming() {
    // Golden request frames, including one the real headset answered.
    uint8_t out[64], want[32];
    bos::Writer w = msgDevInfo();
    size_t n = bos::buildRequest(bos::SVC_DEVICE, bos::DEV_INFO, 0, w.b, w.n, out, sizeof out);
    size_t m = hex("7E FF 80 94 00 06 2D 10 02 01 00 08 00 54 80", want);
    CHECK(n == m && memcmp(out, want, m) == 0);
    n = bos::buildRequest(bos::SVC_DEVICE, bos::DEV_INFO, 0, nullptr, 0, out, sizeof out);
    m = hex("7E FF 80 94 00 04 CE 10 02 01 00 AB 00", want);
    CHECK(n == m && memcmp(out, want, m) == 0);

    // A well-formed frame to damage in the checks below.
    uint8_t cr[32];
    n = bos::buildRequest(bos::SVC_CRUSHER, bos::CRUSHER_READ, 7, nullptr, 0, cr, sizeof cr);
    CHECK(n > 0);

    // Partial frames wait; corrupt ones resync; junk before a frame is skipped.
    bos::Packet p;
    size_t used;
    CHECK(bos::parseFrame(cr, 3, p, used) == bos::PARSE_NEED_MORE);
    CHECK(bos::parseFrame(cr, n - 1, p, used) == bos::PARSE_NEED_MORE);
    CHECK(bos::parseFrame(cr, n, p, used) == bos::PARSE_FRAME && p.svc == 23 && p.method == 2 && p.tag == 7);
    uint8_t bad[32];
    memcpy(bad, cr, n);
    bad[n - 1] ^= 1;
    CHECK(bos::parseFrame(bad, n, p, used) == bos::PARSE_SKIP && used == 1);
    bad[n - 1] ^= 1;
    bad[6] ^= 1;
    CHECK(bos::parseFrame(bad, n, p, used) == bos::PARSE_SKIP && used == 1);
    uint8_t junk = 0x05;
    CHECK(bos::parseFrame(&junk, 1, p, used) == bos::PARSE_SKIP && used == 1);
}

static void testAllowList() {
    uint8_t out[32];
    // Pairing (25), device time set (2,6) and any unknown service or method stay unreachable.
    CHECK(bos::buildRequest(25, 1, 0, nullptr, 0, out, sizeof out) == 0);
    CHECK(bos::buildRequest(2, 6, 0, nullptr, 0, out, sizeof out) == 0);
    CHECK(bos::buildRequest(99, 1, 0, nullptr, 0, out, sizeof out) == 0);
    CHECK(bos::buildRequest(23, 4, 0, nullptr, 0, out, sizeof out) == 0);
    CHECK(bos::buildRequest(4, 8, 0, nullptr, 0, out, sizeof out) == 0);
    CHECK(bos::buildRequest(23, 3, 0, nullptr, 0, out, sizeof out) > 0);
}

static void testRealReplies() {
    HsState s;
    feed(s, 1, 2, 3, "0A 04 10 38 18 01");  // REAL battery
    CHECK(s.battery == 56 && !s.charging);

    feed(s, 1, 4, 1, "08 02 12 05 08 01 10 88 0E 1A 05 08 01 10 88 0E");  // REAL hearing state
    CHECK(s.hearMode == HEAR_ANC && s.ancGain == 1800 && s.awareGain == 1800);
    feed(s, 1, 4, 3, "0A 05 08 01 10 88 0E 12 05 08 01 10 88 0E");  // REAL hearing info
    CHECK(s.ancMax == 1800 && s.awareMax == 1800);

    feed(s, 1, 4, 4, "08 01 10 10");  // REAL: EQ on, Music
    CHECK(s.eqEnabled && s.eqActive == 16);

    // REAL Custom preset: id 0 is omitted from the wire
    feed(s, 1, 4, 5, "10 01 1A 06 43 75 73 74 6F 6D 22 0C 08 32 15 00 00 20 41 1D 8F C2 35 3F 22 0D 08 F4 03 15 00 00 20 41 1D 8F C2 35 3F 22 0D 08 E8 07 15 00 00 20 41 1D 8F C2 35 3F 22 0D 08 B8 17 15 00 00 20 41 1D 8F C2 35 3F 22 0D 08 A8 46 15 00 00 20 41 1D 8F C2 35 3F");
    // REAL Podcast preset
    feed(s, 1, 4, 5, "08 11 1A 07 50 6F 64 63 61 73 74 22 0C 08 32 15 00 00 E0 40 1D 8F C2 35 3F 22 0D 08 F4 03 15 00 00 00 41 1D 8F C2 35 3F 22 0D 08 E8 07 15 00 00 70 41 1D 8F C2 35 3F 22 0D 08 B8 17 15 00 00 80 40 1D 8F C2 35 3F 22 0D 08 A8 46 15 00 00 20 41 1D 8F C2 35 3F");
    CHECK(s.presetCount == 2);
    Preset* c = findPreset(s, 0);
    CHECK(c && !strcmp(c->name, "Custom") && c->bandCount == 5);
    CHECK(c && c->bands[0].freq == 50 && c->bands[4].freq == 9000 && fabs(rawToDb(c->bands[2].raw)) < 0.01);
    Preset* pod = findPreset(s, 17);
    CHECK(pod && !strcmp(pod->name, "Podcast"));
    CHECK(pod && fabs(rawToDb(pod->bands[0].raw) + 3) < 0.01 && fabs(rawToDb(pod->bands[3].raw) + 6) < 0.01);

    feed(s, 1, 23, 1, "08 1F 18 03");  // REAL crusher config
    CHECK(s.crusherMax == 31 && s.crusherFeels == 3 && !s.crusherReadOnly);
    feed(s, 1, 23, 2, "08 01 10 0C 18 02");  // REAL crusher state
    CHECK(s.crusherActive && s.crusherLevel == 12 && s.crusherFeel == 2);
    feed(s, 1, 23, 3, "08 01 10 0B 18 02");  // REAL reply to a set
    CHECK(s.crusherLevel == 11);
    feed(s, 1, 23, 2, "");  // proto3 omits all-default fields, so an empty body means off, 0, 0
    CHECK(!s.crusherActive && s.crusherLevel == 0 && s.crusherFeel == 0);

    feed(s, 1, 4, 10, "08 01");  // REAL sidetone: on, volume 0
    feed(s, 1, 4, 12, "08 18");  // REAL sidetone info
    CHECK(s.sideOn && s.sideVol == 0 && s.sideMax == 24);
    feed(s, 1, 17, 1, "08 01 10 01");  // REAL spatial
    CHECK(s.spatialAvail && s.spatialMode == SPATIAL_OFF);

    feed(s, 2, 4, 7, "00");  // REAL success status
    CHECK(s.lastError == 0);
    feed(s, 2, 4, 7, "05");
    CHECK(s.lastError == 5 && s.lastErrorSvc == 4 && s.lastErrorMethod == 7);
}

static void testWriteBodies() {
    uint8_t want[16];
    bos::Writer w = msgSelectPreset(18);  // the exact body proven on hardware
    size_t n = hex("08 12 10 01", want);
    CHECK(w.n == n && !memcmp(w.b, want, n));
    w = msgSelectPreset(0);  // Custom's id 0 must still be written
    n = hex("08 00 10 01", want);
    CHECK(w.n == n && !memcmp(w.b, want, n));
    w = msgHearMode(HEAR_AWARE);
    n = hex("08 03 10 01", want);
    CHECK(w.n == n && !memcmp(w.b, want, n));
    w = msgHearMode(HEAR_ANC, 900);
    n = hex("08 02 10 01 20 84 07", want);
    CHECK(w.n == n && !memcmp(w.b, want, n));

    // Round trip: a band list we write must parse back to the same values.
    Preset p = {};
    p.id = 0;
    p.bandCount = 5;
    int fr[5] = {50, 500, 1000, 3000, 9000};
    for (int i = 0; i < 5; i++) p.bands[i] = {fr[i], dbToRaw((float)(i - 2)), 0.71f};
    w = msgPresetBands(p, true);
    bos::Reader r(w.b, w.n);
    bos::Field f;
    int bands = 0;
    bool persist = false, activate = false;
    while (r.next(f)) {
        if (f.num == 4) bands++;
        if (f.num == 3) persist = f.v != 0;
        if (f.num == 2) activate = f.v != 0;
    }
    CHECK(bands == 5 && persist && activate);
}

static void testDsp() {
    DspSettings d;
    std::string c = buildConfig(d);
    CHECK(has(c, "PC effects are off") && !has(c, "Filter"));

    d.enabled = true;
    c = buildConfig(d);
    CHECK(has(c, "Device: CRUSHER 1080\n") && has(c, "Device: all\n"));
    CHECK(has(c, "Filter 1: ON PK Fc 420 Hz Gain 1.50 dB Q 1.40"));
    CHECK(has(c, "Filter 3: ON PK Fc 7000 Hz Gain -5.00 dB Q 0.70"));
    CHECK(!has(c, "Copy:"));

    d.bass = 6;
    d.eq[5] = 3;
    c = buildConfig(d);
    CHECK(has(c, "ON LSC Fc 85 Hz Gain 6.00 dB Q 0.70"));
    CHECK(has(c, "ON PK Fc 1000 Hz Gain 3.00 dB Q 1.41"));
    CHECK(has(c, "Preamp: -"));

    // No boost may exceed the headroom left for it.
    Filter fl[MAX_FILTERS];
    int n = collectFilters(d, fl);
    double pre = requiredPreamp(d);
    for (double f = 20; f < 20000; f *= 1.01) CHECK(curveDb(fl, n, f) + pre < 0.3);

    // A filter's response must match the textbook value at its own centre.
    Filter pk = {F_PK, 1000, 6, 1};
    CHECK(fabs(filterDb(pk, 1000) - 6) < 0.01);
    Filter ls = {F_LSC, 100, 6, 0.7f};
    CHECK(fabs(filterDb(ls, 20) - 6) < 0.2 && fabs(filterDb(ls, 5000)) < 0.1);
    Filter hs = {F_HSC, 6000, 4, 0.7f};
    CHECK(fabs(filterDb(hs, 20000) - 4) < 0.6 && fabs(filterDb(hs, 100)) < 0.1);

    // Stereo effects use the syntax in the Equalizer APO reference, and add to the headroom.
    DspSettings w = d;
    w.surround = 40;
    w.ambience = 40;
    c = buildConfig(w);
    CHECK(has(c, "Copy: L=1.200*L+-0.200*R R=1.200*R+-0.200*L"));
    CHECK(has(c, "Copy: XL=1.0*L XR=1.0*R"));
    CHECK(has(c, "Copy: L=L+0.100*XR R=R+0.100*XL"));
    CHECK(requiredPreamp(w) < requiredPreamp(d) - 3.0);
    CHECK(c.find("Channel: XL XR") < c.find("Channel: all\nCopy: L=L+"));

    // Every preset must be recognised again when its values match.
    for (int i = 0; i < FX_PRESET_COUNT; i++) {
        DspSettings p;
        p.clarity = kFxPresets[i].clarity;
        p.bass = kFxPresets[i].bass;
        p.surround = kFxPresets[i].surround;
        p.ambience = kFxPresets[i].ambience;
        CHECK(matchFxPreset(p) == i);
    }
}

static void testPatch() {
    std::string out;
    CHECK(patchConfigTxt("Preamp: -6 dB\nInclude: example.txt\n", out));
    CHECK(has(out, "# Disabled by CrusherFX: Preamp: -6 dB\n"));
    CHECK(has(out, "# Disabled by CrusherFX: Include: example.txt\n"));
    CHECK(has(out, "Include: CrusherFX.txt\n"));

    // The user's own filters stay active, even when they sit next to the demo lines.
    CHECK(patchConfigTxt("Preamp: -6 dB\nInclude: example.txt\nFilter: ON PK Fc 100 Hz Gain 1 dB Q 1\n", out));
    CHECK(has(out, "\nPreamp: -6 dB") || out.rfind("Preamp: -6 dB", 0) == 0);
    CHECK(!has(out, "# Disabled"));
    CHECK(has(out, "Filter: ON PK Fc 100 Hz") && has(out, "Include: CrusherFX.txt\n"));

    // Comments and blank lines survive, and a second run changes nothing.
    std::string first;
    CHECK(patchConfigTxt("# mine\n\nPreamp: -2 dB\n", first));
    CHECK(has(first, "# mine\n\nPreamp: -2 dB\n"));
    CHECK(!patchConfigTxt(first, out));
    CHECK(patchConfigTxt("", out) && out == "Include: CrusherFX.txt\n");
}

int main() {
    testFraming();
    testAllowList();
    testRealReplies();
    testWriteBodies();
    testDsp();
    testPatch();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
