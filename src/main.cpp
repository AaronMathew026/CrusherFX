// CrusherFX: control panel and tray app for the Skullcandy Crusher 1080 ANC.
// The UI is one custom-drawn window. It only repaints on input or headset events, so an idle or
// hidden app runs no code at all.
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

#include "eapo.h"
#include "headset.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ---- palette --------------------------------------------------------------------------------
static const COLORREF C_BG = RGB(20, 21, 24), C_CARD = RGB(30, 32, 36), C_TRACK = RGB(52, 55, 61),
                      C_TEXT = RGB(238, 239, 241), C_MUTED = RGB(160, 164, 172), C_ACCENT = RGB(245, 166, 35),
                      C_ON_ACCENT = RGB(20, 21, 24), C_DISABLED = RGB(95, 98, 105), C_BAD = RGB(240, 110, 95),
                      C_GRID = RGB(46, 48, 54);

// ---- app state ------------------------------------------------------------------------------
enum Page { PAGE_HEADSET, PAGE_EFFECTS, PAGE_SETTINGS };

static HINSTANCE g_inst;
static HWND g_hwnd;
static HICON g_iconLarge, g_iconSmall;
static HsState g_hs;
static DspSettings g_dsp;
static Eapo g_eapo;
static Page g_page = PAGE_HEADSET;
static bool g_closeToTray = true, g_exiting = false;
static wchar_t g_ini[MAX_PATH];
static wchar_t g_dspError[120];
static UINT g_taskbarCreated;

enum { WM_TRAY = WM_APP + 10, WM_SHOW_MAIN = WM_APP + 11 };
enum { T_DEBOUNCE = 1, T_BATTERY = 2, T_DSP = 3 };
enum { TRAY_UID = 1 };

// Slider values waiting for the debounce timer.
static bool g_pendAnc, g_pendCrusher, g_pendSide, g_pendEq;

// ---- widgets --------------------------------------------------------------------------------
enum Kind { W_SEG, W_SLIDER, W_VSLIDER, W_TOGGLE, W_BUTTON };

enum Id {
    ID_NONE, ID_TAB, ID_ANC_MODE, ID_ANC_LEVEL, ID_CR_ON, ID_CR_LEVEL, ID_CR_FEEL, ID_PROFILE,
    ID_EQ0, ID_EQ1, ID_EQ2, ID_EQ3, ID_EQ4, ID_SPATIAL, ID_SIDE,
    ID_FX_ON, ID_FX_TUNING, ID_FX_PRESET, ID_FX_CLARITY, ID_FX_BASS, ID_FX_SURROUND, ID_FX_AMBIENCE,
    ID_G0, ID_G1, ID_G2, ID_G3, ID_G4, ID_G5, ID_G6, ID_G7, ID_G8, ID_G9, ID_G_RESET,
    ID_SET_AUTOSTART, ID_SET_CLOSE, ID_APO_GET, ID_APO_CONFIG, ID_EXIT
};

struct Widget {
    int id;
    Kind kind;
    RECT r;
    int val, minv, maxv;
    const wchar_t* const* segs;
    int nsegs;
    bool disabled;
};

static Widget g_w[64];
static int g_nw, g_captured = -1, g_focus = -1;
static int g_dpi = 96;
static HFONT g_fTitle, g_fBody, g_fSmall;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void makeFonts() {
    if (g_fTitle) { DeleteObject(g_fTitle); DeleteObject(g_fBody); DeleteObject(g_fSmall); }
    auto mk = [](int h, int w) {
        return CreateFontW(-S(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    g_fTitle = mk(15, FW_SEMIBOLD);
    g_fBody = mk(13, FW_NORMAL);
    g_fSmall = mk(11, FW_NORMAL);
}

// ---- settings -------------------------------------------------------------------------------
static int iniInt(const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"app", k, def, g_ini); }
static void iniPut(const wchar_t* k, int v) {
    wchar_t b[16];
    swprintf(b, 16, L"%d", v);
    WritePrivateProfileStringW(L"app", k, b, g_ini);
}

static void loadSettings() {
    wchar_t dir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, dir))) {
        wcscat(dir, L"\\CrusherFX");
        CreateDirectoryW(dir, nullptr);
        swprintf(g_ini, MAX_PATH, L"%ls\\settings.ini", dir);
    }
    g_closeToTray = iniInt(L"closeToTray", 1) != 0;
    g_dsp.enabled = iniInt(L"fxOn", 0) != 0;
    g_dsp.tuning = iniInt(L"fxTuning", 1) != 0;
    g_dsp.clarity = iniInt(L"fxClarity", 0);
    g_dsp.bass = iniInt(L"fxBass", 0);
    g_dsp.surround = iniInt(L"fxSurround", 0);
    g_dsp.ambience = iniInt(L"fxAmbience", 0);
    for (int i = 0; i < EQ_BANDS; i++) {
        wchar_t k[8];
        swprintf(k, 8, L"eq%d", i);
        g_dsp.eq[i] = iniInt(k, 0);
    }
}

static void saveSettings() {
    iniPut(L"closeToTray", g_closeToTray);
    iniPut(L"fxOn", g_dsp.enabled);
    iniPut(L"fxTuning", g_dsp.tuning);
    iniPut(L"fxClarity", g_dsp.clarity);
    iniPut(L"fxBass", g_dsp.bass);
    iniPut(L"fxSurround", g_dsp.surround);
    iniPut(L"fxAmbience", g_dsp.ambience);
    for (int i = 0; i < EQ_BANDS; i++) {
        wchar_t k[8];
        swprintf(k, 8, L"eq%d", i);
        iniPut(k, g_dsp.eq[i]);
    }
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool autostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"CrusherFX", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 20];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 20, L"\"%ls\" --background", exe);
        RegSetValueExW(k, L"CrusherFX", 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"CrusherFX");
    }
    RegCloseKey(k);
}

// ---- headset actions ------------------------------------------------------------------------
using namespace bos;

static void sendHearMode(int mode) {
    g_hs.hearMode = mode;
    hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(mode));
    hsSend(SVC_HEARING, HEAR_MODE_READ, msgSubscribe(false));
}

static void sendAncLevel() {
    int mode = g_hs.hearMode;
    if (mode != HEAR_ANC && mode != HEAR_AWARE) return;
    hsSend(SVC_HEARING, HEAR_MODE_SET, msgHearMode(mode, mode == HEAR_ANC ? g_hs.ancGain : g_hs.awareGain));
}

static void sendCrusherOn(bool on) {
    g_hs.crusherActive = on;
    Writer w;
    w.boolean(1, on);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w);
}

static void sendCrusherLevel() {
    Writer w;
    w.u32(2, (uint32_t)g_hs.crusherLevel);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w);
}

static void sendCrusherFeel(int feel) {
    g_hs.crusherFeel = feel;
    Writer w;
    w.u32(3, (uint32_t)feel);
    hsSend(SVC_CRUSHER, CRUSHER_SET, w);
}

static void sendProfile(int id) {
    g_hs.eqActive = id;
    hsSend(SVC_HEARING, HEAR_PEQ_SET, msgSelectPreset(id));
    hsSend(SVC_HEARING, HEAR_PEQ_STATE, msgSubscribe(false));
}

static void sendCustomEq(bool persist) {
    Preset* p = findPreset(g_hs, 0);
    if (p) hsSend(SVC_HEARING, HEAR_PEQ_SET, msgPresetBands(*p, persist));
}

static void sendSidetone() {
    Writer w;
    w.boolean(1, g_hs.sideVol > 0);
    w.u32(2, (uint32_t)g_hs.sideVol);
    hsSend(SVC_HEARING, HEAR_SIDE_SET, w);
}

static void sendSpatial(int mode) {
    g_hs.spatialMode = mode;
    Writer w;
    w.u32(1, (uint32_t)mode);
    hsSend(SVC_SPATIAL, SPATIAL_SET, w);
}

static void flushPending(bool final) {
    if (g_pendAnc) sendAncLevel();
    if (g_pendCrusher) sendCrusherLevel();
    if (g_pendSide) sendSidetone();
    if (g_pendEq) sendCustomEq(final);
    g_pendAnc = g_pendCrusher = g_pendSide = false;
    if (final) g_pendEq = false;  // otherwise stays marked, so mouse-up stores the final curve
}

// ---- PC effects -----------------------------------------------------------------------------
static void applyDsp() {
    g_dspError[0] = 0;
    if (!g_eapo.installed) return;
    if (g_dsp.enabled && !eapoEnsureInclude(g_eapo)) {
        wcscpy(g_dspError, L"Couldn't edit Equalizer APO's config.txt.");
        return;
    }
    if (!eapoWrite(g_eapo, g_dsp)) wcscpy(g_dspError, L"Couldn't write CrusherFX.txt in the Equalizer APO folder.");
}

static void dspChanged() {
    SetTimer(g_hwnd, T_DSP, 120, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void setFxPreset(int i) {
    const FxPreset& p = kFxPresets[i];
    g_dsp.clarity = p.clarity;
    g_dsp.bass = p.bass;
    g_dsp.surround = p.surround;
    g_dsp.ambience = p.ambience;
    dspChanged();
}

// ---- tray -----------------------------------------------------------------------------------
static const wchar_t* hearName(int m) {
    return m == HEAR_ANC ? L"Quiet" : m == HEAR_AWARE ? L"Aware" : m == HEAR_OFF ? L"ANC off" : L"";
}

static void trayUpdate(DWORD op) {
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof n;
    n.hWnd = g_hwnd;
    n.uID = TRAY_UID;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_iconSmall;
    if (g_hs.connected && g_hs.battery >= 0)
        swprintf(n.szTip, 128, L"CrusherFX\n%d%%%ls  |  %ls  |  Crusher %ls", g_hs.battery, g_hs.charging ? L" charging" : L"",
                 hearName(g_hs.hearMode), g_hs.crusherActive ? L"on" : L"off");
    else
        wcscpy(n.szTip, L"CrusherFX\nHeadphones not connected");
    Shell_NotifyIconW(op, &n);
    if (op == NIM_ADD) {
        n.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
    }
}

static void showMain() {
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void hideMain() {
    ShowWindow(g_hwnd, SW_HIDE);
    // Hand UI pages back to Windows while the panel isn't on screen.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

enum {
    M_OPEN = 1000, M_EXIT, M_FX, M_QUIET, M_AWARE, M_OFF, M_CR_OFF, M_CR_LOW, M_CR_MID, M_CR_HIGH, M_CR_MAX,
    M_PROFILE = 1100
};

static void trayMenu() {
    HMENU m = CreatePopupMenu(), anc = CreatePopupMenu(), cr = CreatePopupMenu(), prof = CreatePopupMenu();
    bool up = g_hs.connected;
    UINT en = up ? MF_ENABLED : MF_GRAYED;
    wchar_t head[64];
    if (up && g_hs.battery >= 0) swprintf(head, 64, L"Crusher 1080 ANC: %d%%", g_hs.battery);
    else wcscpy(head, L"Headphones not connected");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(anc, MF_STRING | en | (g_hs.hearMode == HEAR_ANC ? MF_CHECKED : 0), M_QUIET, L"Quiet");
    AppendMenuW(anc, MF_STRING | en | (g_hs.hearMode == HEAR_AWARE ? MF_CHECKED : 0), M_AWARE, L"Aware");
    AppendMenuW(anc, MF_STRING | en | (g_hs.hearMode == HEAR_OFF ? MF_CHECKED : 0), M_OFF, L"Off");
    AppendMenuW(m, MF_POPUP | en, (UINT_PTR)anc, L"Noise control");

    int cm = g_hs.crusherMax > 0 ? g_hs.crusherMax : 31, cl = g_hs.crusherActive ? g_hs.crusherLevel : -1;
    struct { UINT id; const wchar_t* t; int lo, hi; } rows[] = {
        {M_CR_LOW, L"Low", 1, cm / 3}, {M_CR_MID, L"Medium", cm / 3 + 1, cm * 2 / 3},
        {M_CR_HIGH, L"High", cm * 2 / 3 + 1, cm - 1}, {M_CR_MAX, L"Max", cm, cm}};
    AppendMenuW(cr, MF_STRING | en | (!g_hs.crusherActive ? MF_CHECKED : 0), M_CR_OFF, L"Off");
    for (auto& r : rows)
        AppendMenuW(cr, MF_STRING | en | (cl >= r.lo && cl <= r.hi ? MF_CHECKED : 0), r.id, r.t);
    AppendMenuW(m, MF_POPUP | en, (UINT_PTR)cr, L"Crusher bass");

    for (int i = 0; i < g_hs.presetCount; i++) {
        wchar_t nm[16];
        MultiByteToWideChar(CP_UTF8, 0, g_hs.presets[i].name, -1, nm, 16);
        AppendMenuW(prof, MF_STRING | en | (g_hs.presets[i].id == g_hs.eqActive ? MF_CHECKED : 0), M_PROFILE + i, nm);
    }
    AppendMenuW(m, MF_POPUP | (g_hs.presetCount ? en : MF_GRAYED), (UINT_PTR)prof, L"Sound profile");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_eapo.installed ? 0 : MF_GRAYED) | (g_dsp.enabled ? MF_CHECKED : 0), M_FX, L"PC effects");
    AppendMenuW(m, MF_STRING, M_OPEN, L"Open CrusherFX");
    AppendMenuW(m, MF_STRING, M_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // required so the menu closes when clicking elsewhere
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);  // also destroys the submenus

    auto setLevel = [](int v) {
        if (!g_hs.crusherActive) sendCrusherOn(true);
        g_hs.crusherLevel = v;
        sendCrusherLevel();
    };
    switch (cmd) {
    case M_OPEN: showMain(); break;
    case M_EXIT: g_exiting = true; DestroyWindow(g_hwnd); break;
    case M_FX: g_dsp.enabled = !g_dsp.enabled; dspChanged(); break;
    case M_QUIET: sendHearMode(HEAR_ANC); break;
    case M_AWARE: sendHearMode(HEAR_AWARE); break;
    case M_OFF: sendHearMode(HEAR_OFF); break;
    case M_CR_OFF: sendCrusherOn(false); break;
    case M_CR_LOW: setLevel(cm / 4); break;
    case M_CR_MID: setLevel(cm / 2); break;
    case M_CR_HIGH: setLevel(cm * 3 / 4); break;
    case M_CR_MAX: setLevel(cm); break;
    default:
        if (cmd >= M_PROFILE && cmd < M_PROFILE + g_hs.presetCount) sendProfile(g_hs.presets[cmd - M_PROFILE].id);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
    trayUpdate(NIM_MODIFY);
}

// ---- drawing --------------------------------------------------------------------------------
static HDC g_dc;
static int g_winW;

static void fill(const RECT& r, COLORREF c) {
    SetDCBrushColor(g_dc, c);
    FillRect(g_dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text(const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    DrawTextW(g_dc, s, -1, &r, flags | DT_NOPREFIX);
}

static Widget& add(int id, Kind k, RECT r, int v, int mn, int mx, bool dis = false, const wchar_t* const* segs = nullptr, int ns = 0) {
    Widget& w = g_w[g_nw < 63 ? g_nw++ : 63];
    w = {id, k, r, v, mn, mx, segs, ns, dis};
    return w;
}

static RECT rc(int x, int y, int w, int h) { return RECT{x, y, x + w, y + h}; }

static void drawFocus(const Widget& w) {
    if (g_focus >= 0 && g_focus < g_nw && &g_w[g_focus] == &w) {
        RECT r = w.r;
        InflateRect(&r, S(2), S(2));
        SetDCBrushColor(g_dc, C_TEXT);
        FrameRect(g_dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}

static void drawSeg(const Widget& w) {
    fill(w.r, C_TRACK);
    int n = w.nsegs, W = w.r.right - w.r.left;
    for (int i = 0; i < n; i++) {
        RECT c = {w.r.left + W * i / n, w.r.top, w.r.left + W * (i + 1) / n, w.r.bottom};
        bool on = i == w.val && !w.disabled;
        if (on) fill(c, C_ACCENT);
        text(w.segs[i], c, g_fBody, w.disabled ? C_DISABLED : on ? C_ON_ACCENT : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    drawFocus(w);
}

static void drawSlider(const Widget& w) {
    int range = w.maxv - w.minv;
    double t = range > 0 ? (double)(w.val - w.minv) / range : 0;
    COLORREF fg = w.disabled ? C_DISABLED : C_ACCENT;
    if (w.kind == W_SLIDER) {
        int cy = (w.r.top + w.r.bottom) / 2, x = w.r.left + (int)(t * (w.r.right - w.r.left));
        fill(RECT{w.r.left, cy - S(2), w.r.right, cy + S(2)}, C_TRACK);
        int zero = w.minv < 0 ? w.r.left + (int)((double)(0 - w.minv) / range * (w.r.right - w.r.left)) : w.r.left;
        fill(RECT{x < zero ? x : zero, cy - S(2), x < zero ? zero : x, cy + S(2)}, fg);
        fill(RECT{x - S(4), cy - S(8), x + S(4), cy + S(8)}, w.disabled ? C_DISABLED : C_TEXT);
    } else {
        int cx = (w.r.left + w.r.right) / 2, y = w.r.bottom - (int)(t * (w.r.bottom - w.r.top));
        fill(RECT{cx - S(2), w.r.top, cx + S(2), w.r.bottom}, C_TRACK);
        int zero = w.r.bottom - (int)((double)(0 - w.minv) / range * (w.r.bottom - w.r.top));
        fill(RECT{cx - S(2), y < zero ? y : zero, cx + S(2), y < zero ? zero : y}, fg);
        fill(RECT{cx - S(8), y - S(4), cx + S(8), y + S(4)}, w.disabled ? C_DISABLED : C_TEXT);
    }
    drawFocus(w);
}

static void drawToggle(const Widget& w) {
    bool on = w.val != 0;
    fill(w.r, w.disabled ? C_TRACK : on ? C_ACCENT : C_TRACK);
    int H = w.r.bottom - w.r.top, pad = S(3), k = H - 2 * pad;
    int x = on ? w.r.right - pad - k : w.r.left + pad;
    fill(rc(x, w.r.top + pad, k, k), w.disabled ? C_DISABLED : on ? C_ON_ACCENT : C_TEXT);
    drawFocus(w);
}

static void drawButton(const Widget& w, const wchar_t* label) {
    fill(w.r, C_TRACK);
    text(label, w.r, g_fBody, w.disabled ? C_DISABLED : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawFocus(w);
}

// Row with a caption on the left and a value on the right.
static void caption(int x, int y, int w, const wchar_t* left, const wchar_t* right = nullptr, COLORREF lc = C_TEXT) {
    text(left, rc(x, y, w, S(18)), g_fBody, lc);
    if (right) text(right, rc(x, y, w, S(18)), g_fBody, C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void heading(int x, int& y, int w, const wchar_t* s) {
    text(s, rc(x, y, w, S(20)), g_fTitle, C_TEXT);
    y += S(24);
}

// Display order of the on-board presets, whatever order the headset lists them in.
static const int kProfileOrder[4] = {16, 17, 18, 0};  // Music, Podcast, Movie, Custom (observed ids)
static const wchar_t* const kProfileNames[4] = {L"Music", L"Podcast", L"Movie", L"Custom"};

static void pageHeadset(int x, int y, int w) {
    bool up = g_hs.connected;
    wchar_t v[48];

    // Noise control
    static const wchar_t* const kAnc[] = {L"Quiet", L"Aware", L"Off"};
    heading(x, y, w, L"Noise control");
    int ancSel = g_hs.hearMode == HEAR_ANC ? 0 : g_hs.hearMode == HEAR_AWARE ? 1 : g_hs.hearMode == HEAR_OFF ? 2 : -1;
    drawSeg(add(ID_ANC_MODE, W_SEG, rc(x, y, w, S(30)), ancSel, 0, 2, !up, kAnc, 3));
    y += S(38);
    bool hasLevel = up && (g_hs.hearMode == HEAR_ANC || g_hs.hearMode == HEAR_AWARE);
    int gain = g_hs.hearMode == HEAR_AWARE ? g_hs.awareGain : g_hs.ancGain;
    int gmax = g_hs.hearMode == HEAR_AWARE ? g_hs.awareMax : g_hs.ancMax;
    if (gmax > 0 && hasLevel) swprintf(v, 48, L"%d%%", gain * 100 / gmax);
    else wcscpy(v, L"-");
    caption(x, y, w, g_hs.hearMode == HEAR_AWARE ? L"Aware level" : L"ANC level", v, hasLevel ? C_TEXT : C_DISABLED);
    y += S(20);
    drawSlider(add(ID_ANC_LEVEL, W_SLIDER, rc(x, y, w, S(22)), gain, 0, gmax > 0 ? gmax : 1, !hasLevel || gmax <= 0));
    y += S(34);

    // Crusher
    bool crUp = up && !g_hs.crusherReadOnly;
    heading(x, y, w, L"Crusher bass");
    caption(x, y + S(2), w - S(50), L"Haptic bass", nullptr, crUp ? C_TEXT : C_DISABLED);
    drawToggle(add(ID_CR_ON, W_TOGGLE, rc(x + w - S(44), y, S(44), S(22)), g_hs.crusherActive, 0, 1, !crUp));
    y += S(30);
    if (up && g_hs.crusherMax > 0) swprintf(v, 48, L"%d / %d", g_hs.crusherLevel, g_hs.crusherMax);
    else wcscpy(v, L"-");
    bool lvl = crUp && g_hs.crusherActive;
    caption(x, y, w, L"Intensity", v, lvl ? C_TEXT : C_DISABLED);
    y += S(20);
    drawSlider(add(ID_CR_LEVEL, W_SLIDER, rc(x, y, w, S(22)), g_hs.crusherLevel, 0, g_hs.crusherMax > 0 ? g_hs.crusherMax : 31, !lvl));
    y += S(28);
    static const wchar_t* const kFeel[] = {L"High", L"Medium", L"Low"};
    caption(x, y, w, L"Feel", nullptr, lvl ? C_TEXT : C_DISABLED);
    y += S(20);
    drawSeg(add(ID_CR_FEEL, W_SEG, rc(x, y, w, S(28)), g_hs.crusherFeel, 0, 2, !lvl, kFeel, 3));
    y += S(40);

    // Sound profile
    heading(x, y, w, L"Sound profile");
    int sel = -1;
    for (int i = 0; i < 4; i++)
        if (kProfileOrder[i] == g_hs.eqActive) sel = i;
    drawSeg(add(ID_PROFILE, W_SEG, rc(x, y, w, S(30)), sel, 0, 3, !up || g_hs.presetCount == 0, kProfileNames, 4));
    y += S(38);
    Preset* cp = findPreset(g_hs, g_hs.eqActive >= 0 ? g_hs.eqActive : 16);
    if (cp && cp->bandCount > 0) {
        bool editable = up && g_hs.eqActive == 0;
        int cw = w / cp->bandCount;
        for (int i = 0; i < cp->bandCount; i++) {
            int bx = x + cw * i;
            int db = (int)(rawToDb(cp->bands[i].raw) + (cp->bands[i].raw >= 10 ? 0.5f : -0.5f));
            swprintf(v, 48, db ? L"%+d" : L"%d", db);
            text(v, rc(bx, y, cw, S(16)), g_fSmall, editable ? C_TEXT : C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawSlider(add(ID_EQ0 + i, W_VSLIDER, rc(bx + cw / 2 - S(10), y + S(20), S(20), S(66)), db, -10, 10, !editable));
            int f = cp->bands[i].freq;
            if (f >= 1000) swprintf(v, 48, L"%dk", f / 1000);
            else swprintf(v, 48, L"%d", f);
            text(v, rc(bx, y + S(90), cw, S(16)), g_fSmall, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        y += S(112);
    }

    // Immersive audio and sidetone
    static const wchar_t* const kSp[] = {L"Off", L"Still", L"Motion"};
    bool spUp = up && g_hs.spatialAvail;
    heading(x, y, w, L"Immersive audio");
    int sp = g_hs.spatialMode >= SPATIAL_OFF && g_hs.spatialMode <= SPATIAL_MOTION ? g_hs.spatialMode - 1 : -1;
    drawSeg(add(ID_SPATIAL, W_SEG, rc(x, y, w, S(28)), sp, 0, 2, !spUp, kSp, 3));
    y += S(38);
    if (up && g_hs.sideMax > 0) swprintf(v, 48, L"%d / %d", g_hs.sideVol, g_hs.sideMax);
    else wcscpy(v, L"-");
    caption(x, y, w, L"Sidetone (hear your own voice on calls)", v, up ? C_TEXT : C_DISABLED);
    y += S(20);
    drawSlider(add(ID_SIDE, W_SLIDER, rc(x, y, w, S(22)), g_hs.sideVol, 0, g_hs.sideMax > 0 ? g_hs.sideMax : 1, !up || g_hs.sideMax <= 0));
}

static void drawCurve(RECT r) {
    fill(r, C_CARD);
    Filter fl[MAX_FILTERS];
    int n = collectFilters(g_dsp, fl);
    int H = r.bottom - r.top, W = r.right - r.left, midY = r.top + H / 2;
    const double rangeDb = 15.0;
    HPEN grid = CreatePen(PS_SOLID, 1, C_GRID), line = CreatePen(PS_SOLID, S(2), g_dsp.enabled ? C_ACCENT : C_DISABLED);
    HGDIOBJ old = SelectObject(g_dc, grid);
    for (int db : {-10, 0, 10}) {
        int y = midY - (int)(db / rangeDb * H / 2);
        MoveToEx(g_dc, r.left, y, nullptr);
        LineTo(g_dc, r.right, y);
    }
    for (double f : {100.0, 1000.0, 10000.0}) {
        int x = r.left + (int)(log10(f / 20) / 3 * W);
        MoveToEx(g_dc, x, r.top, nullptr);
        LineTo(g_dc, x, r.bottom);
    }
    SelectObject(g_dc, line);
    for (int x = 0; x <= W; x += S(2)) {
        double f = 20.0 * pow(1000.0, (double)x / W);
        double db = curveDb(fl, n, f);
        if (db > rangeDb) db = rangeDb;
        if (db < -rangeDb) db = -rangeDb;
        int y = midY - (int)(db / rangeDb * H / 2);
        if (x == 0) MoveToEx(g_dc, r.left, y, nullptr);
        else LineTo(g_dc, r.left + x, y);
    }
    SelectObject(g_dc, old);
    DeleteObject(grid);
    DeleteObject(line);
}

static void pageEffects(int x, int y, int w) {
    wchar_t v[96];
    bool have = g_eapo.installed;
    heading(x, y, w, L"PC effects");
    caption(x, y + S(2), w - S(50), have ? L"Apply to everything played on these headphones" : L"Needs Equalizer APO (free)", nullptr, have ? C_TEXT : C_MUTED);
    drawToggle(add(ID_FX_ON, W_TOGGLE, rc(x + w - S(44), y, S(44), S(22)), g_dsp.enabled, 0, 1, !have));
    y += S(30);
    caption(x, y + S(2), w - S(50), L"Crusher 1080 tuning (tames 3 to 10 kHz)");
    drawToggle(add(ID_FX_TUNING, W_TOGGLE, rc(x + w - S(44), y, S(44), S(22)), g_dsp.tuning, 0, 1));
    y += S(32);

    static const wchar_t* kNames[FX_PRESET_COUNT];
    for (int i = 0; i < FX_PRESET_COUNT; i++) kNames[i] = kFxPresets[i].name;
    drawSeg(add(ID_FX_PRESET, W_SEG, rc(x, y, w, S(28)), matchFxPreset(g_dsp), 0, FX_PRESET_COUNT - 1, false, kNames, FX_PRESET_COUNT));
    y += S(36);

    int col = (w - S(20)) / 2;
    swprintf(v, 96, L"%d%%", g_dsp.clarity);
    caption(x, y, col, L"Clarity", v);
    swprintf(v, 96, g_dsp.bass ? L"%+d dB" : L"%d dB", g_dsp.bass);
    caption(x + col + S(20), y, col, L"Bass", v);
    y += S(20);
    drawSlider(add(ID_FX_CLARITY, W_SLIDER, rc(x, y, col, S(22)), g_dsp.clarity, 0, 100));
    drawSlider(add(ID_FX_BASS, W_SLIDER, rc(x + col + S(20), y, col, S(22)), g_dsp.bass, -6, 10));
    y += S(30);
    swprintf(v, 96, L"%d%%", g_dsp.surround);
    caption(x, y, col, L"Surround", v);
    swprintf(v, 96, L"%d%%", g_dsp.ambience);
    caption(x + col + S(20), y, col, L"Ambience", v);
    y += S(20);
    drawSlider(add(ID_FX_SURROUND, W_SLIDER, rc(x, y, col, S(22)), g_dsp.surround, 0, 100));
    drawSlider(add(ID_FX_AMBIENCE, W_SLIDER, rc(x + col + S(20), y, col, S(22)), g_dsp.ambience, 0, 100));
    y += S(34);

    int cw = w / EQ_BANDS;
    static const wchar_t* const kF[EQ_BANDS] = {L"31", L"62", L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k", L"16k"};
    heading(x, y, w, L"Equalizer");
    add(ID_G_RESET, W_BUTTON, rc(x + w - S(64), y - S(26), S(64), S(22)), 0, 0, 0);
    drawButton(g_w[g_nw - 1], L"Reset");
    for (int i = 0; i < EQ_BANDS; i++) {
        int bx = x + cw * i;
        swprintf(v, 96, g_dsp.eq[i] ? L"%+d" : L"%d", g_dsp.eq[i]);
        text(v, rc(bx, y, cw, S(14)), g_fSmall, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawSlider(add(ID_G0 + i, W_VSLIDER, rc(bx + cw / 2 - S(10), y + S(18), S(20), S(84)), g_dsp.eq[i], -12, 12));
        text(kF[i], rc(bx, y + S(106), cw, S(14)), g_fSmall, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    y += S(128);

    drawCurve(rc(x, y, w, S(78)));
    y += S(84);
    if (g_dspError[0]) {
        text(g_dspError, rc(x, y, w, S(18)), g_fSmall, C_BAD);
    } else if (have) {
        swprintf(v, 96, L"Headroom %.1f dB, to prevent clipping. There is no limiter.", requiredPreamp(g_dsp));
        text(v, rc(x, y, w, S(18)), g_fSmall, C_MUTED);
    } else {
        text(L"Install Equalizer APO from the Settings tab to hear these.", rc(x, y, w, S(18)), g_fSmall, C_MUTED);
    }
}

static void pageSettings(int x, int y, int w) {
    heading(x, y, w, L"Background");
    caption(x, y + S(2), w - S(50), L"Start with Windows, hidden in the tray");
    drawToggle(add(ID_SET_AUTOSTART, W_TOGGLE, rc(x + w - S(44), y, S(44), S(22)), autostartEnabled(), 0, 1));
    y += S(32);
    caption(x, y + S(2), w - S(50), L"Closing the window keeps it in the tray");
    drawToggle(add(ID_SET_CLOSE, W_TOGGLE, rc(x + w - S(44), y, S(44), S(22)), g_closeToTray, 0, 1));
    y += S(44);

    heading(x, y, w, L"Equalizer APO");
    RECT note = rc(x, y, w, S(68));
    text(g_eapo.installed ? L"Installed. If PC effects make no difference, open the device list, tick your Crusher 1080 under Playback devices, then restart Windows."
                          : L"PC effects run inside Windows through this free add-on, so CrusherFX itself does no audio processing. The headset controls work without it.",
         note, g_fBody, C_MUTED, DT_LEFT | DT_WORDBREAK);
    y += S(70);
    if (g_eapo.installed) {
        add(ID_APO_CONFIG, W_BUTTON, rc(x, y, w, S(30)), 0, 0, 0);
        drawButton(g_w[g_nw - 1], L"Open Equalizer APO device list");
    } else {
        add(ID_APO_GET, W_BUTTON, rc(x, y, w, S(30)), 0, 0, 0);
        drawButton(g_w[g_nw - 1], L"Download Equalizer APO");
    }
    y += S(46);

    heading(x, y, w, L"Headphones");
    wchar_t v[96];
    if (g_hs.connected && g_hs.model[0]) {
        wchar_t model[40], fw[16];
        MultiByteToWideChar(CP_UTF8, 0, g_hs.model, -1, model, 40);
        MultiByteToWideChar(CP_UTF8, 0, g_hs.firmware, -1, fw, 16);
        swprintf(v, 96, L"%ls, firmware %ls", model, fw);
    } else {
        wcscpy(v, L"Not connected");
    }
    text(v, rc(x, y, w, S(18)), g_fBody, C_MUTED);
    y += S(26);
    if (g_hs.lastError) {
        swprintf(v, 96, L"Last rejected call: service %d, method %d, status %d", g_hs.lastErrorSvc, g_hs.lastErrorMethod, g_hs.lastError);
        text(v, rc(x, y, w, S(18)), g_fBody, C_BAD);
        y += S(26);
    }
    y += S(10);
    add(ID_EXIT, W_BUTTON, rc(x, y, w, S(30)), 0, 0, 0);
    drawButton(g_w[g_nw - 1], L"Exit CrusherFX");
}

static void paint(HDC dc, int W, int H) {
    g_dc = dc;
    g_winW = W;
    g_nw = 0;
    SetBkMode(dc, TRANSPARENT);
    fill(RECT{0, 0, W, H}, C_BG);

    int m = S(20), w = W - 2 * m, y = S(14);
    text(L"CrusherFX", rc(m, y, w, S(24)), g_fTitle, C_TEXT);
    wchar_t st[64];
    if (g_hs.connected && g_hs.battery >= 0) swprintf(st, 64, L"Connected  |  %d%%%ls", g_hs.battery, g_hs.charging ? L" charging" : L"");
    else if (g_hs.connected) wcscpy(st, L"Connected");
    else wcscpy(st, L"Headphones not connected");
    text(st, rc(m, y, w, S(24)), g_fBody, g_hs.connected ? C_MUTED : C_BAD, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    y += S(32);

    static const wchar_t* const kTabs[] = {L"Headset", L"PC effects", L"Settings"};
    drawSeg(add(ID_TAB, W_SEG, rc(m, y, w, S(30)), g_page, 0, 2, false, kTabs, 3));
    y += S(44);

    switch (g_page) {
    case PAGE_HEADSET: pageHeadset(m, y, w); break;
    case PAGE_EFFECTS: pageEffects(m, y, w); break;
    case PAGE_SETTINGS: pageSettings(m, y, w); break;
    }
}

// ---- input ----------------------------------------------------------------------------------
static void onValue(Widget& w, int v, bool final) {
    switch (w.id) {
    case ID_TAB: g_page = (Page)v; g_focus = -1; break;
    case ID_ANC_MODE: sendHearMode(v == 0 ? HEAR_ANC : v == 1 ? HEAR_AWARE : HEAR_OFF); break;
    case ID_ANC_LEVEL:
        (g_hs.hearMode == HEAR_AWARE ? g_hs.awareGain : g_hs.ancGain) = v;
        g_pendAnc = true;
        break;
    case ID_CR_ON: sendCrusherOn(v != 0); break;
    case ID_CR_LEVEL: g_hs.crusherLevel = v; g_pendCrusher = true; break;
    case ID_CR_FEEL: sendCrusherFeel(v); break;
    case ID_PROFILE: sendProfile(kProfileOrder[v]); break;
    case ID_EQ0: case ID_EQ1: case ID_EQ2: case ID_EQ3: case ID_EQ4: {
        Preset* p = findPreset(g_hs, 0);
        if (p && w.id - ID_EQ0 < p->bandCount) {
            p->bands[w.id - ID_EQ0].raw = dbToRaw((float)v);
            g_pendEq = true;
        }
        break;
    }
    case ID_SPATIAL: sendSpatial(v + 1); break;
    case ID_SIDE: g_hs.sideVol = v; g_hs.sideOn = v > 0; g_pendSide = true; break;
    case ID_FX_ON: g_dsp.enabled = v != 0; dspChanged(); break;
    case ID_FX_TUNING: g_dsp.tuning = v != 0; dspChanged(); break;
    case ID_FX_PRESET: setFxPreset(v); break;
    case ID_FX_CLARITY: g_dsp.clarity = v; dspChanged(); break;
    case ID_FX_BASS: g_dsp.bass = v; dspChanged(); break;
    case ID_FX_SURROUND: g_dsp.surround = v; dspChanged(); break;
    case ID_FX_AMBIENCE: g_dsp.ambience = v; dspChanged(); break;
    case ID_SET_AUTOSTART: setAutostart(v != 0); break;
    case ID_SET_CLOSE: g_closeToTray = v != 0; saveSettings(); break;
    default:
        if (w.id >= ID_G0 && w.id <= ID_G9) { g_dsp.eq[w.id - ID_G0] = v; dspChanged(); }
    }
    if (final) flushPending(true);
    else if (g_pendAnc || g_pendCrusher || g_pendSide || g_pendEq) SetTimer(g_hwnd, T_DEBOUNCE, 80, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    trayUpdate(NIM_MODIFY);
}

static void onClickButton(Widget& w) {
    switch (w.id) {
    case ID_G_RESET:
        memset(g_dsp.eq, 0, sizeof g_dsp.eq);
        dspChanged();
        break;
    case ID_APO_GET:
        ShellExecuteW(nullptr, L"open", L"https://sourceforge.net/projects/equalizerapo/", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ID_APO_CONFIG: {
        std::wstring exe = g_eapo.installDir + L"\\Configurator.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_EXIT: g_exiting = true; DestroyWindow(g_hwnd); break;
    }
}

static int sliderValueAt(const Widget& w, int mx, int my) {
    double t = w.kind == W_SLIDER ? (double)(mx - w.r.left) / (w.r.right - w.r.left)
                                  : (double)(w.r.bottom - my) / (w.r.bottom - w.r.top);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return w.minv + (int)(t * (w.maxv - w.minv) + 0.5);
}

static int hitTest(int x, int y) {
    POINT p = {x, y};
    for (int i = 0; i < g_nw; i++)
        if (PtInRect(&g_w[i].r, p)) return i;
    return -1;
}

static void onMouseDown(int x, int y) {
    int i = hitTest(x, y);
    if (i < 0) return;
    Widget& w = g_w[i];
    if (w.disabled) return;
    g_focus = -1;
    switch (w.kind) {
    case W_SEG: {
        int seg = (x - w.r.left) * w.nsegs / (w.r.right - w.r.left);
        if (seg >= w.nsegs) seg = w.nsegs - 1;
        onValue(w, seg, true);
        break;
    }
    case W_TOGGLE: onValue(w, !w.val, true); break;
    case W_BUTTON: onClickButton(w); break;
    case W_SLIDER: case W_VSLIDER:
        g_captured = i;
        SetCapture(g_hwnd);
        w.val = sliderValueAt(w, x, y);
        onValue(w, w.val, false);
        break;
    }
}

static void onKey(WPARAM key) {
    if (key == VK_TAB) {
        if (g_nw == 0) return;
        int dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        for (int n = 0; n < g_nw; n++) {
            g_focus = (g_focus + dir + g_nw) % g_nw;
            if (!g_w[g_focus].disabled) break;
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_focus < 0 || g_focus >= g_nw) return;
    Widget& w = g_w[g_focus];
    if (w.disabled) return;
    int step = 0;
    if (key == VK_LEFT || key == VK_DOWN) step = -1;
    if (key == VK_RIGHT || key == VK_UP) step = 1;
    if (key == VK_SPACE || key == VK_RETURN) {
        if (w.kind == W_TOGGLE) onValue(w, !w.val, true);
        else if (w.kind == W_BUTTON) onClickButton(w);
        return;
    }
    if (!step) return;
    if (w.kind == W_SEG) {
        int v = w.val + step;
        if (v >= 0 && v < w.nsegs) onValue(w, v, true);
    } else if (w.kind == W_SLIDER || w.kind == W_VSLIDER) {
        int range = w.maxv - w.minv, inc = range > 64 ? range / 20 : 1, v = w.val + step * inc;
        if (v < w.minv) v = w.minv;
        if (v > w.maxv) v = w.maxv;
        onValue(w, v, true);
    }
}

static void onWheel(int delta, int sx, int sy) {
    POINT p = {sx, sy};
    ScreenToClient(g_hwnd, &p);
    int i = hitTest(p.x, p.y);
    if (i < 0) return;
    Widget& w = g_w[i];
    if (w.disabled || (w.kind != W_SLIDER && w.kind != W_VSLIDER)) return;
    int range = w.maxv - w.minv, inc = range > 64 ? range / 20 : 1;
    int v = w.val + (delta > 0 ? inc : -inc);
    if (v < w.minv) v = w.minv;
    if (v > w.maxv) v = w.maxv;
    onValue(w, v, true);
}

// ---- window ---------------------------------------------------------------------------------
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg != 0) {  // Explorer restarted: the tray icon has to be re-added
        trayUpdate(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, cr.right, cr.bottom);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT cr;
        GetClientRect(h, &cr);
        paint((HDC)wp, cr.right, cr.bottom);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: onMouseDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSEMOVE:
        if (g_captured >= 0 && g_captured < g_nw) {
            Widget& w = g_w[g_captured];
            int v = sliderValueAt(w, (short)LOWORD(lp), (short)HIWORD(lp));
            if (v != w.val) { w.val = v; onValue(w, v, false); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_captured >= 0 && g_captured < g_nw) {
            ReleaseCapture();
            Widget w = g_w[g_captured];
            g_captured = -1;
            KillTimer(h, T_DEBOUNCE);
            onValue(w, w.val, true);
        }
        return 0;
    case WM_CAPTURECHANGED: g_captured = -1; return 0;
    case WM_MOUSEWHEEL: onWheel(GET_WHEEL_DELTA_WPARAM(wp), (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: onKey(wp); return 0;
    case WM_TIMER:
        if (wp == T_DEBOUNCE) {
            KillTimer(h, T_DEBOUNCE);
            flushPending(false);
        } else if (wp == T_DSP) {
            KillTimer(h, T_DSP);
            applyDsp();
            saveSettings();
            InvalidateRect(h, nullptr, FALSE);
        } else if (wp == T_BATTERY) {
            Writer none;
            hsSend(SVC_DEVICE, DEV_BATTERY, none);
        }
        return 0;
    case WM_HS_UP:
        g_hs = HsState();
        g_hs.connected = true;
        SetTimer(h, T_BATTERY, 5 * 60 * 1000, nullptr);
        trayUpdate(NIM_MODIFY);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_HS_DOWN:
        g_hs = HsState();
        KillTimer(h, T_BATTERY);
        trayUpdate(NIM_MODIFY);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_HS_PACKET: {
        HsPacket* p = (HsPacket*)lp;
        // A drag owns its value until release, so a late echo can't make the slider jump back.
        bool dragging = g_captured >= 0;
        if (!dragging) hsApply(g_hs, p->p);
        hsFree(p);
        trayUpdate(NIM_MODIFY);
        if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP:
            if (IsWindowVisible(h)) hideMain();
            else showMain();
            break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: trayMenu(); break;
        }
        return 0;
    case WM_SHOW_MAIN: showMain(); return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        makeFonts();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (g_closeToTray && !g_exiting) hideMain();
        else DestroyWindow(h);
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) saveSettings();
        return 0;
    case WM_DESTROY:
        saveSettings();
        {
            NOTIFYICONDATAW n = {};
            n.cbSize = sizeof n;
            n.hWnd = h;
            n.uID = TRAY_UID;
            Shell_NotifyIconW(NIM_DELETE, &n);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    static const wchar_t* kClass = L"CrusherFXWindow";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"CrusherFX-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kClass, nullptr);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_SHOW_MAIN, 0, 0);
        }
        return 0;
    }
    bool background = wcsstr(cmd, L"--background") != nullptr;

    g_inst = inst;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    loadSettings();
    g_eapo = eapoDetect();

    g_iconLarge = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_dpi = GetDpiForSystem();
    makeFonts();
    RECT r = {0, 0, S(420), S(668)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    g_hwnd = CreateWindowExW(0, kClass, L"CrusherFX", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    g_dpi = GetDpiForWindow(g_hwnd);
    makeFonts();

    trayUpdate(NIM_ADD);
    if (g_dsp.enabled) applyDsp();
    hsStart(g_hwnd);
    if (background) hideMain();
    else showMain();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    hsStop();
    CloseHandle(single);
    return 0;
}
