// Live test against real headphones, using the same Bluetooth code as the app.
//   hwtest          read everything and print it (changes nothing)
//   hwtest --write  also change each setting briefly, check it took effect, and restore it
//   hwtest --watch  print state whenever the headset pushes a change, for 30 s (turn the wheel!)
#include <stdio.h>
#include <wchar.h>

#include "../src/headset.h"

static HsState g;
static int g_packets, g_ups, g_errors;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HS_UP) { g = HsState(); g.connected = true; g_ups++; return 0; }
    if (m == WM_HS_DOWN) { g.connected = false; return 0; }
    if (m == WM_HS_PACKET) {
        HsPacket* p = (HsPacket*)l;
        hsApply(g, p->p);
        if (p->p.type == bos::TYPE_STATUS && p->p.pbLen && p->p.pb[0]) g_errors++;
        g_packets++;
        hsFree(p);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        DWORD now = GetTickCount();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - now, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
}

static int g_fail;
static void check(bool ok, const char* what) {
    printf("  %-48s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) g_fail++;
}

static void show() {
    printf("model=%s fw=%s authRequired=%d battery=%d charging=%d\n", g.model, g.firmware, g.authRequired, g.battery, g.charging);
    printf("hearing mode=%d anc=%d/%d aware=%d/%d\n", g.hearMode, g.ancGain, g.ancMax, g.awareGain, g.awareMax);
    printf("eq enabled=%d active=%d presets=%d\n", g.eqEnabled, g.eqActive, g.presetCount);
    for (int i = 0; i < g.presetCount; i++) {
        printf("  [%2d] %-8s", g.presets[i].id, g.presets[i].name);
        for (int b = 0; b < g.presets[i].bandCount; b++) printf(" %d:%+.0f", g.presets[i].bands[b].freq, rawToDb(g.presets[i].bands[b].raw));
        printf("\n");
    }
    printf("sidetone on=%d vol=%d/%d spatial avail=%d mode=%d\n", g.sideOn, g.sideVol, g.sideMax, g.spatialAvail, g.spatialMode);
    printf("crusher active=%d level=%d/%d feel=%d/%d readOnly=%d\n", g.crusherActive, g.crusherLevel, g.crusherMax, g.crusherFeel, g.crusherFeels, g.crusherReadOnly);
}

using namespace bos;

// Relies only on subscriptions and call replies, as the app does: no extra reads between steps.
static void writeTests() {
    HsState o = g;
    printf("write tests (every setting is restored):\n");

    hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(HEAR_AWARE));
    pump(1000);
    check(g.hearMode == HEAR_AWARE, "ANC -> Aware, pushed back by subscription");
    hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(o.hearMode));
    pump(1000);
    check(g.hearMode == o.hearMode, "ANC restored");

    if (o.hearMode == HEAR_ANC) {
        hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(HEAR_ANC, o.ancMax / 2));
        pump(1000);
        hsSend(SVC_HEARING, HEAR_MODE_READ, msgSubscribe(false));
        pump(500);
        check(g.ancGain == o.ancMax / 2, "ANC level -> 50%");
        hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(HEAR_ANC, o.ancGain));
        pump(1000);
        hsSend(SVC_HEARING, HEAR_MODE_READ, msgSubscribe(false));
        pump(500);
        check(g.ancGain == o.ancGain, "ANC level restored");
    }

    Writer w;
    w.u32(2, (uint32_t)(o.crusherLevel > 0 ? o.crusherLevel - 1 : 1));
    hsSend(SVC_CRUSHER, CRUSHER_SET, w);
    pump(800);
    check(g.crusherLevel != o.crusherLevel, "Crusher intensity changes by one step");
    Writer w2;
    w2.u32(2, (uint32_t)o.crusherLevel);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w2);
    pump(800);
    check(g.crusherLevel == o.crusherLevel, "Crusher intensity restored");

    Writer w3;
    w3.u32(3, (uint32_t)((o.crusherFeel + 1) % (o.crusherFeels > 0 ? o.crusherFeels : 3)));
    hsSend(SVC_CRUSHER, CRUSHER_SET, w3);
    pump(800);
    check(g.crusherFeel != o.crusherFeel, "Crusher feel changes");
    Writer w4;
    w4.u32(3, (uint32_t)o.crusherFeel);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w4);
    pump(800);
    check(g.crusherFeel == o.crusherFeel, "Crusher feel restored");

    Writer w5;
    w5.boolean(1, !o.crusherActive);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w5);
    pump(800);
    check(g.crusherActive != o.crusherActive, "Crusher on/off toggles");
    Writer w6;
    w6.boolean(1, o.crusherActive);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w6);
    pump(800);
    check(g.crusherActive == o.crusherActive, "Crusher on/off restored");

    hsSend(SVC_HEARING, HEAR_PEQ_SET, msgSelectPreset(17));
    pump(1000);
    check(g.eqActive == 17, "Profile -> Podcast, pushed back by subscription");

    // Custom EQ: write a recognisable curve without persisting it, read it back, then put it back.
    Preset* c = findPreset(o, 0);
    if (c) {
        Preset t = *c;
        t.bands[1].raw = dbToRaw(3);
        t.bands[3].raw = dbToRaw(-2);
        hsSend(SVC_HEARING, HEAR_PEQ_SET, msgPresetBands(t, false));
        pump(1000);
        check(g.eqActive == 0, "Custom EQ write activates Custom");
        g.presetCount = 0;
        hsSend(SVC_HEARING, HEAR_PEQ_CONF, Writer());
        pump(1200);
        Preset* r = findPreset(g, 0);
        check(r && rawToDb(r->bands[1].raw) == 3 && rawToDb(r->bands[3].raw) == -2, "Custom EQ curve reads back");
        hsSend(SVC_HEARING, HEAR_PEQ_SET, msgPresetBands(*c, false));
        pump(800);
    }
    hsSend(SVC_HEARING, HEAR_PEQ_SET, msgSelectPreset(o.eqActive));
    pump(1000);
    check(g.eqActive == o.eqActive, "Profile restored");

    Writer s;
    s.u32(1, SPATIAL_STILL);
    hsSend(SVC_SPATIAL, SPATIAL_SET, s);
    pump(1500);
    hsSend(SVC_SPATIAL, SPATIAL_READ, msgSubscribe(false));
    pump(500);
    check(g.spatialMode == SPATIAL_STILL, "Immersive audio -> Still");
    Writer s2;
    s2.u32(1, (uint32_t)o.spatialMode);
    hsSend(SVC_SPATIAL, SPATIAL_SET, s2);
    pump(1500);
    hsSend(SVC_SPATIAL, SPATIAL_READ, msgSubscribe(false));
    pump(500);
    check(g.spatialMode == o.spatialMode, "Immersive audio restored");

    Writer t1;
    t1.boolean(1, true);
    t1.u32(2, 6);
    hsSend(SVC_HEARING, HEAR_SIDE_SET, t1);
    pump(800);
    hsSend(SVC_HEARING, HEAR_SIDE_READ, msgSubscribe(false));
    pump(500);
    check(g.sideVol == 6, "Sidetone -> 6");
    Writer t2;
    t2.boolean(1, o.sideOn);
    t2.u32(2, (uint32_t)o.sideVol);
    hsSend(SVC_HEARING, HEAR_SIDE_SET, t2);
    pump(800);
    hsSend(SVC_HEARING, HEAR_SIDE_READ, msgSubscribe(false));
    pump(500);
    check(g.sideVol == o.sideVol && g.sideOn == o.sideOn, "Sidetone restored");

    check(g_errors == 0, "headset rejected no call");
    printf("final state:\n");
    show();
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool write = argc > 1 && !wcscmp(argv[1], L"--write");
    bool watch = argc > 1 && !wcscmp(argv[1], L"--watch");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CrusherFXHwTest";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    DWORD t0 = GetTickCount();
    hsStart(h);
    while (!g.connected && GetTickCount() - t0 < 70000) pump(50);
    if (!g.connected) {
        printf("could not connect within 70 s (connect error %d; -1 means Windows reports no connected Crusher)\n", hsLastConnectError());
        return 2;
    }
    printf("link up after %lu ms\n", (unsigned long)(GetTickCount() - t0));
    pump(1800);
    printf("packets received: %d\n", g_packets);
    show();
    if (write) writeTests();
    if (watch) {
        printf("watching for 30 s. Turn the wheel or press the headset's buttons.\n");
        HsState last = g;
        DWORD end = GetTickCount() + 30000;
        while (GetTickCount() < end) {
            pump(100);
            if (g.crusherLevel != last.crusherLevel || g.crusherActive != last.crusherActive || g.hearMode != last.hearMode || g.eqActive != last.eqActive) {
                printf("  push: crusher active=%d level=%d, hearing=%d, eq=%d\n", g.crusherActive, g.crusherLevel, g.hearMode, g.eqActive);
                last = g;
            }
        }
    }
    hsStop();
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "done");
    return g_fail ? 1 : 0;
}
