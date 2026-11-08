#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <stdlib.h>
#include <wchar.h>

#include "headset.h"

// The RFCOMM service the headset advertises as BOS_RPC. Windows resolves the channel through SDP.
static const GUID kBosRpc = {0x54C5BF4C, 0xC52F, 0x4635, {0x98, 0xDC, 0xEE, 0xBF, 0x97, 0xC6, 0x72, 0x6F}};

static HWND g_notify;
static HANDLE g_thread, g_stop;
static CRITICAL_SECTION g_lock;
static SOCKET g_sock = INVALID_SOCKET;
static uint8_t g_tag;
static volatile int g_lastConnectError = -1;  // -1: headset not seen yet; otherwise a Winsock error
int hsLastConnectError() { return g_lastConnectError; }

// Finds the paired headset, but only reports it while Windows is connected to it. Connecting a
// socket to a sleeping or absent headset would page it and could pull it away from a phone.
static bool findConnectedHeadset(BTH_ADDR& addr) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS q = {};
    q.dwSize = sizeof q;
    q.fReturnAuthenticated = TRUE;
    q.fReturnRemembered = TRUE;
    q.fReturnConnected = TRUE;
    BLUETOOTH_DEVICE_INFO d = {};
    d.dwSize = sizeof d;
    HBLUETOOTH_DEVICE_FIND h = BluetoothFindFirstDevice(&q, &d);
    if (!h) return false;
    bool found = false;
    do {
        if (d.fConnected && wcsstr(d.szName, L"CRUSHER 1080")) {
            addr = d.Address.ullLong;
            found = true;
            break;
        }
    } while (BluetoothFindNextDevice(h, &d));
    BluetoothFindDeviceClose(h);
    return found;
}

static SOCKET openLink(BTH_ADDR addr) {
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) {
        g_lastConnectError = WSAGetLastError();
        return s;
    }
    SOCKADDR_BTH a = {};
    a.addressFamily = AF_BTH;
    a.btAddr = addr;
    a.serviceClassId = kBosRpc;
    if (connect(s, (sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
        g_lastConnectError = WSAGetLastError();
        closesocket(s);
        return INVALID_SOCKET;
    }
    g_lastConnectError = 0;
    DWORD timeout = 1000;  // a stalled link must never freeze the UI thread for long
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof timeout);
    return s;
}

bool hsSend(int svc, int method, const bos::Writer& body) {
    uint8_t frame[300];
    EnterCriticalSection(&g_lock);
    bool ok = false;
    if (g_sock != INVALID_SOCKET) {
        size_t n = bos::buildRequest(svc, method, g_tag++, body.b, body.n, frame, sizeof frame);
        ok = n > 0 && send(g_sock, (const char*)frame, (int)n, 0) == (int)n;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

void hsRequestInitialState() {
    using namespace bos;
    Writer none;
    Writer sub = msgSubscribe(true);
    hsSend(SVC_DEVICE, DEV_INFO, msgDevInfo());  // the official app always sends this first
    hsSend(SVC_DEVICE, DEV_BATTERY, none);
    hsSend(SVC_HEARING, HEAR_MODE_INFO, none);
    hsSend(SVC_HEARING, HEAR_MODE_READ, sub);
    hsSend(SVC_HEARING, HEAR_PEQ_CONF, none);
    hsSend(SVC_HEARING, HEAR_PEQ_STATE, sub);
    hsSend(SVC_HEARING, HEAR_SIDE_INFO, none);
    hsSend(SVC_HEARING, HEAR_SIDE_READ, sub);
    hsSend(SVC_SPATIAL, SPATIAL_READ, sub);
    hsSend(SVC_CRUSHER, CRUSHER_CONFIG, none);
    hsSend(SVC_CRUSHER, CRUSHER_READ, sub);
}

static void postPacket(const bos::Packet& p) {
    HsPacket* h = (HsPacket*)malloc(sizeof(HsPacket) + p.pbLen);
    if (!h) return;
    h->p = p;
    memcpy(h->data, p.pb, p.pbLen);
    h->p.pb = h->data;
    if (!PostMessageW(g_notify, WM_HS_PACKET, 0, (LPARAM)h)) free(h);
}

void hsFree(HsPacket* p) { free(p); }

static void readLoop(SOCKET s) {
    uint8_t buf[2048];
    size_t have = 0;
    for (;;) {
        int got = recv(s, (char*)buf + have, (int)(sizeof buf - have), 0);  // sleeps until data arrives
        if (got <= 0) return;
        have += (size_t)got;
        size_t off = 0;
        for (;;) {
            bos::Packet p;
            size_t used = 0;
            bos::ParseResult r = bos::parseFrame(buf + off, have - off, p, used);
            if (r == bos::PARSE_NEED_MORE) break;
            if (r == bos::PARSE_FRAME) postPacket(p);
            off += used;
        }
        have -= off;
        if (off && have) memmove(buf, buf + off, have);
        if (have == sizeof buf) have = 0;  // unparseable flood: drop it and resync
    }
}

static DWORD WINAPI linkThread(void*) {
    DWORD failDelay = 0;
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        BTH_ADDR addr = 0;
        DWORD wait = 10000;  // the only poll: one local lookup every 10 s while the headset is away
        if (findConnectedHeadset(addr)) {
            SOCKET s = openLink(addr);
            if (s == INVALID_SOCKET) {
                // Connected for audio but refusing the control link. Each attempt pages the headset
                // and can take a minute, so back off rather than keep pressing it.
                failDelay = failDelay ? (failDelay * 2 > 300000 ? 300000 : failDelay * 2) : 15000;
                wait = failDelay;
            } else {
                failDelay = 0;
                EnterCriticalSection(&g_lock);
                g_sock = s;
                g_tag = 0;
                LeaveCriticalSection(&g_lock);
                PostMessageW(g_notify, WM_HS_UP, 0, 0);
                hsRequestInitialState();
                readLoop(s);
                EnterCriticalSection(&g_lock);
                if (g_sock != INVALID_SOCKET) closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                LeaveCriticalSection(&g_lock);
                PostMessageW(g_notify, WM_HS_DOWN, 0, 0);
            }
        }
        WaitForSingleObject(g_stop, wait);
    }
    return 0;
}

void hsStart(HWND notify) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&g_lock);
    g_notify = notify;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 64 * 1024, linkThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void hsStop() {
    SetEvent(g_stop);
    EnterCriticalSection(&g_lock);
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);  // wakes the blocked recv
        g_sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_lock);
    // A connect() in progress can't be interrupted, and exiting the process ends it safely.
    WaitForSingleObject(g_thread, 1500);
}
