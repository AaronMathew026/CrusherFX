// Bluetooth link to the headset: one blocking reader thread that sleeps until data arrives.
#pragma once
#include <windows.h>

#include "model.h"

enum {
    WM_HS_UP = WM_APP + 1,    // link opened
    WM_HS_DOWN = WM_APP + 2,  // link lost
    WM_HS_PACKET = WM_APP + 3 // lParam = HsPacket*, the receiver frees it with hsFree
};

struct HsPacket {
    bos::Packet p;
    uint8_t data[1];
};

void hsStart(HWND notify);
void hsStop();
bool hsSend(int svc, int method, const bos::Writer& body);
void hsFree(HsPacket* p);
void hsRequestInitialState();
int hsLastConnectError();  // -1 until Windows reports the headset as connected, then a Winsock error or 0
