// Headset state, updated from decoded BOS-RPC packets, and builders for the write calls.
// Field meanings marked "observed" were read off a real Crusher 1080 ANC (firmware 1.2.0).
#pragma once
#include "proto.h"

enum { HEAR_OFF = 1, HEAR_ANC = 2, HEAR_AWARE = 3 };
enum { SPATIAL_OFF = 1, SPATIAL_STILL = 2, SPATIAL_MOTION = 3 };
enum { MAX_PRESETS = 8, MAX_BANDS = 5 };

struct Band { int freq; float raw; float q; };

struct Preset {
    int id;
    char name[16];
    Band bands[MAX_BANDS];
    int bandCount;
};

// Observed: flat presets store raw gain 10 on a 0..20 scale in 21 steps, so raw 10 is 0 dB.
inline float rawToDb(float raw) { return raw - 10.0f; }
inline float dbToRaw(float db) { return db + 10.0f; }

struct HsState {
    bool connected = false;
    char model[40] = "";
    char firmware[16] = "";
    bool authRequired = false;

    int battery = -1;
    bool charging = false;

    int hearMode = 0;
    int ancGain = 0, ancMax = 0, awareGain = 0, awareMax = 0;

    bool eqEnabled = false;
    int eqActive = -1;
    Preset presets[MAX_PRESETS] = {};
    int presetCount = 0;

    bool sideOn = false;
    int sideVol = 0, sideMax = 0;

    bool spatialAvail = false;
    int spatialMode = 0;

    bool crusherActive = false;
    int crusherLevel = 0, crusherMax = 0, crusherFeel = 0, crusherFeels = 0;
    bool crusherReadOnly = false;

    int lastError = 0;  // non-zero status from the most recent failed call
    int lastErrorSvc = 0, lastErrorMethod = 0;
};

inline void copyStr(char* dst, size_t cap, const bos::Field& f) {
    size_t n = f.len < cap - 1 ? f.len : cap - 1;
    memcpy(dst, f.p, n);
    dst[n] = 0;
}

inline Preset* findPreset(HsState& s, int id) {
    for (int i = 0; i < s.presetCount; i++)
        if (s.presets[i].id == id) return &s.presets[i];
    return nullptr;
}

// Reads a {preset, gain} pair. proto3 omits zero values, so a present message with no gain field means 0.
inline void readModeItem(const bos::Field& f, int& gain) {
    bos::Reader r(f.p, f.len);
    bos::Field g;
    gain = 0;
    while (r.next(g))
        if (g.num == 2 && g.wire == 0) gain = (int)g.v;
}

inline void parsePresetConf(HsState& s, const bos::Packet& p) {
    Preset t = {};
    bos::Reader r(p.pb, p.pbLen);
    bos::Field f;
    while (r.next(f)) {
        if (f.num == 1 && f.wire == 0) t.id = (int)f.v;
        else if (f.num == 3 && f.wire == 2) copyStr(t.name, sizeof(t.name), f);
        else if (f.num == 4 && f.wire == 2 && t.bandCount < MAX_BANDS) {
            Band b = {};
            bos::Reader br(f.p, f.len);
            bos::Field g;
            while (br.next(g)) {
                if (g.num == 1 && g.wire == 0) b.freq = (int)g.v;
                else if (g.num == 2 && g.wire == 5) b.raw = g.f;
                else if (g.num == 3 && g.wire == 5) b.q = g.f;
            }
            t.bands[t.bandCount++] = b;
        }
    }
    Preset* e = findPreset(s, t.id);
    if (!e && s.presetCount < MAX_PRESETS) e = &s.presets[s.presetCount++];
    if (e) *e = t;
}

// Applies any response or push. Pushes use the same layout, so calls and pushes share one path.
inline void hsApply(HsState& s, const bos::Packet& p) {
    using namespace bos;
    if (p.type == TYPE_STATUS) {
        if (p.pbLen > 0 && p.pb[0] != 0) {
            s.lastError = p.pb[0];
            s.lastErrorSvc = p.svc;
            s.lastErrorMethod = p.method;
        }
        return;
    }
    if (p.type != TYPE_RESPONSE) return;
    Reader r(p.pb, p.pbLen);
    Field f;

    if (p.svc == SVC_DEVICE && p.method == DEV_INFO) {
        s.authRequired = false;
        while (r.next(f)) {
            if (f.num == 4 && f.wire == 2) copyStr(s.model, sizeof(s.model), f);
            else if (f.num == 5 && f.wire == 2) copyStr(s.firmware, sizeof(s.firmware), f);
            else if (f.num == 12 && f.wire == 0) s.authRequired = f.v != 0;
            // Field 9 is the serial number. It is never stored or logged.
        }
    } else if (p.svc == SVC_DEVICE && p.method == DEV_BATTERY) {
        while (r.next(f)) {
            if (f.num != 1 || f.wire != 2) continue;
            Reader b(f.p, f.len);
            Field g;
            int level = 0, status = 0;
            while (b.next(g)) {
                if (g.num == 2) level = (int)g.v;
                else if (g.num == 3) status = (int)g.v;
            }
            s.battery = level;
            s.charging = status == 2;
            break;  // the first entry is this device
        }
    } else if (p.svc == SVC_HEARING && p.method == HEAR_MODE_READ) {
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.hearMode = (int)f.v;
            else if (f.num == 2 && f.wire == 2) readModeItem(f, s.ancGain);
            else if (f.num == 3 && f.wire == 2) readModeItem(f, s.awareGain);
        }
    } else if (p.svc == SVC_HEARING && p.method == HEAR_MODE_INFO) {
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 2) readModeItem(f, s.ancMax);
            else if (f.num == 2 && f.wire == 2) readModeItem(f, s.awareMax);
        }
    } else if (p.svc == SVC_HEARING && p.method == HEAR_PEQ_STATE) {
        s.eqEnabled = false;
        s.eqActive = 0;  // proto3 omits id 0, which is Custom
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.eqEnabled = f.v != 0;
            else if (f.num == 2 && f.wire == 0) s.eqActive = (int)f.v;
        }
    } else if (p.svc == SVC_HEARING && p.method == HEAR_PEQ_CONF) {
        parsePresetConf(s, p);
    } else if (p.svc == SVC_HEARING && p.method == HEAR_SIDE_READ) {
        s.sideOn = false;
        s.sideVol = 0;
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.sideOn = f.v != 0;
            else if (f.num == 2 && f.wire == 0) s.sideVol = (int)f.v;
        }
    } else if (p.svc == SVC_HEARING && p.method == HEAR_SIDE_INFO) {
        while (r.next(f))
            if (f.num == 1 && f.wire == 0) s.sideMax = (int)f.v;
    } else if (p.svc == SVC_SPATIAL && (p.method == SPATIAL_READ || p.method == SPATIAL_SET)) {
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.spatialAvail = f.v != 0;
            else if (f.num == 2 && f.wire == 0) s.spatialMode = (int)f.v;
        }
    } else if (p.svc == SVC_CRUSHER && p.method == CRUSHER_CONFIG) {
        s.crusherReadOnly = false;
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.crusherMax = (int)f.v;
            else if (f.num == 2 && f.wire == 0) s.crusherReadOnly = f.v != 0;
            else if (f.num == 3 && f.wire == 0) s.crusherFeels = (int)f.v;
        }
    } else if (p.svc == SVC_CRUSHER && (p.method == CRUSHER_READ || p.method == CRUSHER_SET)) {
        s.crusherActive = false;
        s.crusherLevel = 0;
        s.crusherFeel = 0;
        while (r.next(f)) {
            if (f.num == 1 && f.wire == 0) s.crusherActive = f.v != 0;
            else if (f.num == 2 && f.wire == 0) s.crusherLevel = (int)f.v;
            else if (f.num == 3 && f.wire == 0) s.crusherFeel = (int)f.v;
        }
    }
}

// ---- write-call bodies ----------------------------------------------------------------------
inline bos::Writer msgHearMode(int mode, int gain = -1) {
    bos::Writer w;
    w.u32(1, (uint32_t)mode);
    w.boolean(2, true);
    if (gain >= 0) w.u32(4, (uint32_t)gain);
    return w;
}

inline bos::Writer msgSelectPreset(int id) {
    bos::Writer w;
    w.u32(1, (uint32_t)id);
    w.boolean(2, true);
    return w;
}

inline bos::Writer msgPresetBands(const Preset& p, bool persist) {
    bos::Writer w;
    w.u32(1, (uint32_t)p.id);
    w.boolean(2, true);
    if (persist) w.boolean(3, true);
    for (int i = 0; i < p.bandCount; i++) {
        bos::Writer b;
        b.u32(1, (uint32_t)p.bands[i].freq);
        b.f32(2, p.bands[i].raw);
        b.f32(3, p.bands[i].q);
        w.msg(4, b);
    }
    return w;
}

inline bos::Writer msgSubscribe(bool on) {
    bos::Writer w;
    if (on) w.boolean(1, true);
    return w;
}

inline bos::Writer msgDevInfo() {
    bos::Writer w;
    w.u32(1, 0);  // "unspecified" must be explicit: body 08 00, as the official app sends it
    return w;
}
