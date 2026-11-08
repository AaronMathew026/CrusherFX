// BOS-RPC wire protocol used by the Skullcandy Crusher 1080 ANC.
// Pure code with no OS dependencies, so tests/selftest.cpp can cover all of it.
//
//   Frame: 7E FF 80 94 <len16 BE> <CRC-8/ROHC of bytes 0..5> body <CRC-16/MCRF4XX of bytes 1..end of body, BE>
//   Body : flags service method tag protobuf...
//
// Layout credit: the Earshot project (MIT). Verified here against real hardware.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bos {

inline uint8_t crc8(const uint8_t* d, size_t n) {
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0xE0) : (uint8_t)(c >> 1);
    }
    return c;
}

inline uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

// ---- call addresses -------------------------------------------------------------------------
enum { SVC_DEVICE = 2, SVC_HEARING = 4, SVC_SPATIAL = 17, SVC_CRUSHER = 23 };
enum { DEV_INFO = 1, DEV_BATTERY = 3 };
enum {
    HEAR_MODE_READ = 1, HEAR_MODE_SET = 2, HEAR_MODE_INFO = 3,
    HEAR_PEQ_STATE = 4, HEAR_PEQ_CONF = 5, HEAR_PEQ_INFO = 6, HEAR_PEQ_SET = 7,
    HEAR_SIDE_READ = 10, HEAR_SIDE_SET = 11, HEAR_SIDE_INFO = 12,
};
enum { SPATIAL_READ = 1, SPATIAL_SET = 2 };
enum { CRUSHER_CONFIG = 1, CRUSHER_READ = 2, CRUSHER_SET = 3 };

// Method numbers are per service and other numbers may reach firmware update or pairing calls.
// Nothing outside this list can be sent, whatever the caller asks for.
inline bool isAllowed(int svc, int m) {
    switch (svc) {
    case SVC_DEVICE:  return m == DEV_INFO || m == DEV_BATTERY;
    case SVC_HEARING: return (m >= HEAR_MODE_READ && m <= HEAR_PEQ_SET) || (m >= HEAR_SIDE_READ && m <= HEAR_SIDE_INFO);
    case SVC_SPATIAL: return m == SPATIAL_READ || m == SPATIAL_SET;
    case SVC_CRUSHER: return m == CRUSHER_CONFIG || m == CRUSHER_READ || m == CRUSHER_SET;
    }
    return false;
}

// ---- protobuf writer ------------------------------------------------------------------------
struct Writer {
    uint8_t b[256];
    size_t n = 0;
    void byte(uint8_t v) { if (n < sizeof(b)) b[n++] = v; }
    void varint(uint32_t v) {
        while (v >= 0x80) { byte((uint8_t)(v | 0x80)); v >>= 7; }
        byte((uint8_t)v);
    }
    void u32(int field, uint32_t v) { varint((uint32_t)field << 3); varint(v); }
    void boolean(int field, bool v) { u32(field, v ? 1 : 0); }
    void f32(int field, float v) {
        varint(((uint32_t)field << 3) | 5);
        uint32_t u;
        memcpy(&u, &v, 4);
        for (int i = 0; i < 4; i++) byte((uint8_t)(u >> (8 * i)));
    }
    void msg(int field, const Writer& w) {
        varint(((uint32_t)field << 3) | 2);
        varint((uint32_t)w.n);
        for (size_t i = 0; i < w.n; i++) byte(w.b[i]);
    }
};

// ---- protobuf reader ------------------------------------------------------------------------
struct Field {
    int num = 0, wire = 0;
    uint64_t v = 0;           // wire 0
    float f = 0;              // wire 5
    const uint8_t* p = nullptr;  // wire 2
    size_t len = 0;
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    Reader(const uint8_t* d, size_t n) : p(d), end(d + n) {}
    bool varint(uint64_t& out) {
        out = 0;
        for (int s = 0; s < 64 && p < end; s += 7) {
            uint8_t c = *p++;
            out |= (uint64_t)(c & 0x7F) << s;
            if (!(c & 0x80)) return true;
        }
        return false;
    }
    bool next(Field& f) {
        if (p >= end) return false;
        uint64_t key;
        if (!varint(key)) return false;
        f.num = (int)(key >> 3);
        f.wire = (int)(key & 7);
        switch (f.wire) {
        case 0: return varint(f.v);
        case 1: if (end - p < 8) return false; p += 8; return true;
        case 2: {
            uint64_t l;
            if (!varint(l) || l > (uint64_t)(end - p)) return false;
            f.p = p; f.len = (size_t)l; p += l;
            return true;
        }
        case 5: if (end - p < 4) return false; memcpy(&f.f, p, 4); p += 4; return true;
        }
        return false;
    }
};

// ---- framing --------------------------------------------------------------------------------
enum { TYPE_REQUEST = 0, TYPE_RESPONSE = 1, TYPE_STATUS = 2, TYPE_ABORT = 3 };

// Builds a request frame. Returns its size, or 0 if the call is not allow-listed or won't fit.
inline size_t buildRequest(int svc, int method, uint8_t tag, const uint8_t* pb, size_t pbLen, uint8_t* out, size_t cap) {
    size_t body = 4 + pbLen;
    if (!isAllowed(svc, method) || cap < 9 + body) return 0;
    out[0] = 0x7E; out[1] = 0xFF; out[2] = 0x80; out[3] = 0x94;
    out[4] = (uint8_t)(body >> 8); out[5] = (uint8_t)body;
    out[6] = crc8(out, 6);
    out[7] = 0x10; out[8] = (uint8_t)svc; out[9] = (uint8_t)method; out[10] = tag;
    if (pbLen) memcpy(out + 11, pb, pbLen);
    uint16_t fcs = crc16(out + 1, 6 + body);
    out[7 + body] = (uint8_t)(fcs >> 8);
    out[8 + body] = (uint8_t)fcs;
    return 9 + body;
}

struct Packet {
    int type, svc, method, tag;
    bool final;
    const uint8_t* pb;  // points into the input buffer
    size_t pbLen;
};

enum ParseResult { PARSE_NEED_MORE, PARSE_FRAME, PARSE_SKIP };

// Reads one frame at d. PARSE_SKIP means the bytes aren't a valid frame: drop `consumed` and retry.
inline ParseResult parseFrame(const uint8_t* d, size_t n, Packet& out, size_t& consumed) {
    consumed = 0;
    if (n == 0) return PARSE_NEED_MORE;
    if (d[0] != 0x7E) { consumed = 1; return PARSE_SKIP; }
    if (n < 7) return PARSE_NEED_MORE;
    if (crc8(d, 6) != d[6]) { consumed = 1; return PARSE_SKIP; }
    size_t len = ((size_t)d[4] << 8) | d[5];
    if (n < 9 + len) return PARSE_NEED_MORE;
    uint16_t fcs = (uint16_t)((d[7 + len] << 8) | d[8 + len]);
    if (crc16(d + 1, 6 + len) != fcs) { consumed = 1; return PARSE_SKIP; }
    consumed = 9 + len;
    if (d[3] != 0x94 || len < 4) return PARSE_SKIP;  // well-formed but not cleartext RPC
    out.type = (d[7] >> 2) & 3;
    out.final = (d[7] & 0x10) != 0;
    out.svc = d[8]; out.method = d[9]; out.tag = d[10];
    out.pb = d + 11; out.pbLen = len - 4;
    return PARSE_FRAME;
}

}  // namespace bos
