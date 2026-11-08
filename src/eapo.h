// Equalizer APO integration: find the install, patch config.txt once, write our include file.
#pragma once
#include <windows.h>

#include <string>

#include "dsp.h"

struct Eapo {
    bool installed = false;
    std::wstring configDir;
    std::wstring installDir;
};

inline std::wstring regString(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = L"";
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

inline Eapo eapoDetect() {
    Eapo e;
    e.configDir = regString(L"ConfigPath");
    e.installDir = regString(L"InstallPath");
    e.installed = !e.configDir.empty() && GetFileAttributesW(e.configDir.c_str()) != INVALID_FILE_ATTRIBUTES;
    return e;
}

inline bool readFileText(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    out.resize(size);
    BOOL ok = size == 0 || ReadFile(h, &out[0], size, &got, nullptr);
    CloseHandle(h);
    if (ok) out.resize(got);
    return ok;
}

// Replaces the file in one step, so Equalizer APO's watcher never sees a half-written config.
inline bool writeFileAtomic(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Adds our include to config.txt, keeping the original as config.txt.crusherfx.bak the first time.
inline bool eapoEnsureInclude(const Eapo& e) {
    std::wstring cfg = e.configDir + L"\\config.txt";
    std::string text, patched;
    if (!readFileText(cfg, text)) text.clear();
    if (!patchConfigTxt(text, patched)) return true;
    std::wstring bak = cfg + L".crusherfx.bak";
    if (GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(cfg.c_str(), bak.c_str(), TRUE))
        return false;
    return writeFileAtomic(cfg, patched);
}

inline bool eapoWrite(const Eapo& e, const DspSettings& d) {
    static std::string last;
    std::string text = buildConfig(d);
    if (text == last) return true;  // an unchanged file must not trigger an audio-engine reload
    if (!writeFileAtomic(e.configDir + L"\\CrusherFX.txt", text)) return false;
    last = text;
    return true;
}
