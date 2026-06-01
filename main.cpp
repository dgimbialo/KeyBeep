// KeyBeep - plays a configurable sound on a global hotkey (tray application)
// Pure WinAPI + MIDI — no external dependencies, Windows 10/11 x64
// Build: see build.bat  (requires Visual Studio 2017/2019/2022)
// Install path: %LOCALAPPDATA%\KeyBeep\KeyBeep.exe
// Settings:    HKCU\Software\KeyBeep
// Autostart:   HKCU\Software\Microsoft\Windows\CurrentVersion\Run

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include "resource.h"
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <strsafe.h>
#include <math.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// ---- Constants ----
#define WM_TRAYICON         (WM_USER + 1)
#define IDI_TRAY            1
#define ID_TRAY_SHOW        2001
#define ID_TRAY_EXIT        2002
#define ID_TRAY_AUTOSTART   2003
#define ID_TRAY_INSTALL     2004
#define ID_TRAY_UNINSTALL   2005
#define ID_BTN_SET_KEY      3001
#define ID_BTN_TEST         3002
#define ID_BTN_CLEAR        3003
#define ID_COMBO_SOUND      3004
#define ID_STATIC_HOTKEY    3005
#define TIMER_WAITING_KEY   4001

// ---- Sound types ----
enum SoundType {
    SOUND_BEEP_LOW = 0,
    SOUND_BEEP_MID,
    SOUND_BEEP_HIGH,
    SOUND_DOUBLE_BEEP,
    SOUND_COUNT
};

const wchar_t* SOUND_NAMES[] = {
    L"Beep low (400Hz)",
    L"Beep medium (800Hz)",
    L"Beep high (1200Hz)",
    L"Double Beep"
};

// ---- Global state ----
struct AppState {
    HWND        hwndMain;
    HHOOK       hKeyboardHook;
    NOTIFYICONDATA nid;

    // Hotkey config
    UINT        triggerVK;          // Virtual key code
    bool        needCtrl;
    bool        needAlt;
    bool        needShift;
    bool        needWin;

    // Sound config
    SoundType   soundType;

    // UI state
    bool        waitingForKey;      // capturing next keypress as hotkey
    HWND        hwndStatus;         // label showing current hotkey
    HWND        hwndCombo;
    HWND        hwndBtnSet;
    HWND        hwndBtnTest;
    HWND        hwndBtnClear;
    HWND        hwndCheckCtrl;
    HWND        hwndCheckAlt;
    HWND        hwndCheckShift;
    HWND        hwndCheckWin;

    bool        enabled;

    // Persistent MIDI Out handle — opened once, reused for every note
    HMIDIOUT    hMidiOut;
} g;

// ---- Forward declarations ----
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
void PlayConfiguredSound();
void UpdateHotkeyLabel();
void ShowTrayMenu(HWND hwnd);
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void SaveSettings();
void LoadSettings();
wchar_t* VKToName(UINT vk, wchar_t* buf, int buflen);
bool IsAutoStartEnabled();
void SetAutoStart(bool enable);
bool SelfInstall();
bool SelfUninstall();
bool IsInstalledInstance();

// ---- Registry helpers ----
static const wchar_t* REG_KEY       = L"Software\\KeyBeep";
static const wchar_t* REG_RUN_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_NAME      = L"KeyBeep";
static const wchar_t* INSTALL_SUBDIR = L"KeyBeep";
static const wchar_t* EXE_NAME      = L"KeyBeep.exe";

// Returns install path: %LOCALAPPDATA%\KeyBeep\KeyBeep.exe
static void GetInstallPath(wchar_t* buf, DWORD bufcch) {
    wchar_t appdata[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", appdata, MAX_PATH);
    StringCchPrintfW(buf, bufcch, L"%s\\%s\\%s", appdata, INSTALL_SUBDIR, EXE_NAME);
}

// Returns install directory: %LOCALAPPDATA%\KeyBeep
static void GetInstallDir(wchar_t* buf, DWORD bufcch) {
    wchar_t appdata[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", appdata, MAX_PATH);
    StringCchPrintfW(buf, bufcch, L"%s\\%s", appdata, INSTALL_SUBDIR);
}

// Checks if current exe is already in the install location
bool IsInstalledInstance() {
    wchar_t installPath[MAX_PATH] = {};
    GetInstallPath(installPath, MAX_PATH);
    wchar_t currentPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, currentPath, MAX_PATH);
    return (_wcsicmp(currentPath, installPath) == 0);
}

bool IsAutoStartEnabled() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    wchar_t val[MAX_PATH] = {};
    DWORD sz = sizeof(val);
    LONG res = RegQueryValueExW(hk, APP_NAME, NULL, NULL, (BYTE*)val, &sz);
    RegCloseKey(hk);
    return (res == ERROR_SUCCESS && val[0] != 0);
}

void SetAutoStart(bool enable) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t installPath[MAX_PATH] = {};
        GetInstallPath(installPath, MAX_PATH);
        // Wrap in quotes for paths with spaces
        wchar_t quoted[MAX_PATH + 4] = {};
        StringCchPrintfW(quoted, MAX_PATH + 4, L"\"%s\"", installPath);
        RegSetValueExW(hk, APP_NAME, 0, REG_SZ,
            (BYTE*)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

// Copy exe to %LOCALAPPDATA%\KeyBeep\ and add autostart
bool SelfInstall() {
    wchar_t installDir[MAX_PATH]  = {};
    wchar_t installPath[MAX_PATH] = {};
    GetInstallDir(installDir, MAX_PATH);
    GetInstallPath(installPath, MAX_PATH);

    // Create directory
    if (!CreateDirectoryW(installDir, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            MessageBoxW(NULL, L"Failed to create installation directory.",
                APP_NAME, MB_ICONERROR);
            return false;
        }
    }

    // Copy current exe to install path
    wchar_t currentPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, currentPath, MAX_PATH);

    if (_wcsicmp(currentPath, installPath) != 0) {
        if (!CopyFileW(currentPath, installPath, FALSE)) {
            MessageBoxW(NULL, L"Failed to copy file.\nTry running as administrator.",
                APP_NAME, MB_ICONERROR);
            return false;
        }
    }

    SetAutoStart(true);
    return true;
}

// Remove autostart entry and delete installed files
bool SelfUninstall() {
    SetAutoStart(false);

    wchar_t installPath[MAX_PATH] = {};
    wchar_t installDir[MAX_PATH]  = {};
    GetInstallPath(installPath, MAX_PATH);
    GetInstallDir(installDir, MAX_PATH);

    // Schedule file deletion after exit (can't delete running exe directly)
    // Use cmd /c ping as delay trick — pure WinAPI, no external tools needed
    wchar_t currentPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, currentPath, MAX_PATH);

    if (_wcsicmp(currentPath, installPath) == 0) {
        // Running from install dir: schedule self-deletion via cmd
        wchar_t cmd[1024] = {};
        StringCchPrintfW(cmd, 1024,
            L"cmd.exe /c ping 127.0.0.1 -n 2 >nul & del /f /q \"%s\" & rd /s /q \"%s\"",
            installPath, installDir);
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    } else {
        // Not running from install dir — just delete the installed copy
        DeleteFileW(installPath);
        RemoveDirectoryW(installDir);
    }
    return true;
}

void SaveSettings() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD val;
        val = g.triggerVK;    RegSetValueExW(hk, L"VK",      0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g.needCtrl;     RegSetValueExW(hk, L"Ctrl",    0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g.needAlt;      RegSetValueExW(hk, L"Alt",     0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g.needShift;    RegSetValueExW(hk, L"Shift",   0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g.needWin;      RegSetValueExW(hk, L"Win",     0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = (DWORD)g.soundType; RegSetValueExW(hk, L"Sound", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g.enabled;      RegSetValueExW(hk, L"Enabled", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hk);
    }
}

void LoadSettings() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val, sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"VK",      NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.triggerVK  = val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Ctrl",    NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.needCtrl   = !!val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Alt",     NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.needAlt    = !!val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Shift",   NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.needShift  = !!val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Win",     NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.needWin    = !!val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Sound",   NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.soundType  = (SoundType)val;
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hk, L"Enabled", NULL, NULL, (BYTE*)&val, &sz) == ERROR_SUCCESS) g.enabled    = !!val;
        RegCloseKey(hk);
    }
}

// ---- MIDI Out init/cleanup ----
static void InitMidiOut() {
    if (g.hMidiOut) return;
    // Try MIDI_MAPPER first, then device 0 — same strategy as PiobMasterPro
    if (midiOutOpen(&g.hMidiOut, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g.hMidiOut = NULL;
        if (midiOutOpen(&g.hMidiOut, 0, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            g.hMidiOut = NULL;
    }
    if (g.hMidiOut) {
        // Program Change ch0: use instrument 80 (Lead 1 square) – a clean buzzy tone
        // 0xC0 = Program Change status for ch0; data1 = program number
        DWORD pc = (DWORD)0xC0 | ((DWORD)80 << 8);
        midiOutShortMsg(g.hMidiOut, pc);
        // CC#7 = Channel Volume, max (127)
        DWORD vol = (DWORD)0xB0 | ((DWORD)7 << 8) | ((DWORD)127 << 16);
        midiOutShortMsg(g.hMidiOut, vol);
    }
}

static void CloseMidiOut() {
    if (!g.hMidiOut) return;
    // All notes/sound off before closing
    midiOutShortMsg(g.hMidiOut, 0xB0 | (120 << 8) | (0 << 16)); // All Sound Off
    midiOutShortMsg(g.hMidiOut, 0xB0 | (123 << 8) | (0 << 16)); // All Notes Off
    midiOutClose(g.hMidiOut);
    g.hMidiOut = NULL;
}

// ---- Sound player (persistent MIDI Out handle) ----
// Key insight from PiobMasterPro: open HMIDIOUT ONCE at startup and reuse.
// Opening/closing per note causes failures after any system audio event.

static int FreqToMidiNote(int freq) {
    double note = 69.0 + 12.0 * log((double)freq / 440.0) / log(2.0);
    int n = (int)(note + 0.5);
    if (n < 0) n = 0; if (n > 127) n = 127;
    return n;
}

static void PlayMidiTone(int freq, int durationMs) {
    if (!g.hMidiOut) return;
    BYTE note = (BYTE)FreqToMidiNote(freq);
    // Note On ch0
    midiOutShortMsg(g.hMidiOut, (DWORD)0x90 | ((DWORD)note << 8) | (110UL << 16));
    Sleep((DWORD)durationMs);
    // Note Off ch0
    midiOutShortMsg(g.hMidiOut, (DWORD)0x80 | ((DWORD)note << 8) | (64UL << 16));
}

static DWORD WINAPI SoundThreadProc(LPVOID param) {
    SoundType st = (SoundType)(DWORD_PTR)param;
    switch (st) {
    case SOUND_BEEP_LOW:           PlayMidiTone(400,  80); break;
    case SOUND_BEEP_MID:           PlayMidiTone(800,  80); break;
    case SOUND_BEEP_HIGH:          PlayMidiTone(1200, 80); break;
    case SOUND_DOUBLE_BEEP:        PlayMidiTone(800, 60); Sleep(30); PlayMidiTone(1000, 60); break;
    default:                       PlayMidiTone(800, 80); break;
    }
    return 0;
}

void PlayConfiguredSound() {
    // Snapshot soundType before spawning – avoids race with settings changes
    DWORD_PTR st = (DWORD_PTR)g.soundType;
    HANDLE hThread = CreateThread(NULL, 0, SoundThreadProc, (LPVOID)st, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

// ---- VK name helper ----
// Returns true if vk is a modifier key (Ctrl/Alt/Shift/Win)
static bool IsModifierVK(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
           vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
           vk == VK_LWIN    || vk == VK_RWIN;
}
// Returns true if vk belongs to Ctrl group
static bool IsCtrlVK(UINT vk)  { return vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL; }
// Returns true if vk belongs to Alt group
static bool IsAltVK(UINT vk)   { return vk==VK_MENU||vk==VK_LMENU||vk==VK_RMENU; }
// Returns true if vk belongs to Shift group
static bool IsShiftVK(UINT vk) { return vk==VK_SHIFT||vk==VK_LSHIFT||vk==VK_RSHIFT; }
// Returns true if vk belongs to Win group
static bool IsWinVK(UINT vk)   { return vk==VK_LWIN||vk==VK_RWIN; }

wchar_t* VKToName(UINT vk, wchar_t* buf, int buflen) {
    if (vk == 0) { StringCchCopyW(buf, buflen, L"(not set)"); return buf; }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    // Mark extended keys so GetKeyNameTextW returns distinct names
    if (vk == VK_LEFT   || vk == VK_RIGHT  || vk == VK_UP    || vk == VK_DOWN   ||
        vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME  || vk == VK_END    ||
        vk == VK_PRIOR  || vk == VK_NEXT   || vk == VK_NUMLOCK ||
        vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_RSHIFT ||
        vk == VK_DIVIDE || vk == VK_LWIN   || vk == VK_RWIN)
        sc |= 0x100;
    LONG lParam = sc << 16;
    if (!GetKeyNameTextW(lParam, buf, buflen) || buf[0] == 0) {
        StringCchPrintfW(buf, buflen, L"VK 0x%02X", vk);
    }
    return buf;
}

// ---- Update hotkey label ----
void UpdateHotkeyLabel() {
    if (!g.hwndStatus) return;
    wchar_t keyName[64] = {};
    VKToName(g.triggerVK, keyName, 64);

    wchar_t label[256] = {};
    if (g.triggerVK == 0) {
        StringCchCopyW(label, 256, L"Click [Set Key] and press the desired key");
    } else {
        wchar_t combo[128] = {};
        if (g.needCtrl)  StringCchCatW(combo, 128, L"Ctrl+");
        if (g.needAlt)   StringCchCatW(combo, 128, L"Alt+");
        if (g.needShift) StringCchCatW(combo, 128, L"Shift+");
        if (g.needWin)   StringCchCatW(combo, 128, L"Win+");
        StringCchCatW(combo, 128, keyName);
        StringCchPrintfW(label, 256, L"Hotkey: %s", combo);
    }
    SetWindowTextW(g.hwndStatus, label);
}

// ---- Low-level keyboard hook ----
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

        if (g.waitingForKey) {
            // Capture mode: accept ANY key including standalone modifiers (Ctrl, Alt, Shift, Win)
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                UINT vk = kb->vkCode;
                g.waitingForKey = false;
                g.triggerVK = vk;

                // Read modifier state but EXCLUDE the key being pressed itself
                // so that pressing Alt alone gives needAlt=false, triggerVK=VK_LMENU
                bool rawCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool rawAlt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
                bool rawShift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
                bool rawWin   = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

                g.needCtrl  = rawCtrl  && !IsCtrlVK(vk);
                g.needAlt   = rawAlt   && !IsAltVK(vk);
                g.needShift = rawShift && !IsShiftVK(vk);
                g.needWin   = rawWin   && !IsWinVK(vk);

                // Sync checkboxes
                SendMessageW(g.hwndCheckCtrl,  BM_SETCHECK, g.needCtrl  ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(g.hwndCheckAlt,   BM_SETCHECK, g.needAlt   ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(g.hwndCheckShift, BM_SETCHECK, g.needShift ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(g.hwndCheckWin,   BM_SETCHECK, g.needWin   ? BST_CHECKED : BST_UNCHECKED, 0);

                UpdateHotkeyLabel();
                SetWindowTextW(g.hwndBtnSet, L"Set Key");
                KillTimer(g.hwndMain, TIMER_WAITING_KEY);
                SaveSettings();
                return 1; // consume the key
            }
            return CallNextHookEx(g.hKeyboardHook, nCode, wParam, lParam);
        }

        // Normal mode: check hotkey match on KEYDOWN
        if (g.enabled && g.triggerVK != 0 &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
        {
            if (kb->vkCode == g.triggerVK) {
                // When triggerVK IS a modifier, exclude it from the modifier state check
                // (it will always be "pressed" since it IS the key being pressed)
                bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
                bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
                bool win   = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

                if (IsCtrlVK(g.triggerVK))  ctrl  = false;
                if (IsAltVK(g.triggerVK))   alt   = false;
                if (IsShiftVK(g.triggerVK)) shift = false;
                if (IsWinVK(g.triggerVK))   win   = false;

                if (ctrl == g.needCtrl && alt == g.needAlt &&
                    shift == g.needShift && win == g.needWin)
                {
                    PostMessageW(g.hwndMain, WM_USER + 10, 0, 0);
                }
            }
        }
    }
    return CallNextHookEx(g.hKeyboardHook, nCode, wParam, lParam);
}

// ---- Tray icon ----
void AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g.nid, sizeof(g.nid));
    g.nid.cbSize           = sizeof(NOTIFYICONDATA);
    g.nid.hWnd             = hwnd;
    g.nid.uID              = IDI_TRAY;
    g.nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_TRAYICON;
    g.nid.hIcon            = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_MAIN));
    if (!g.nid.hIcon) g.nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    StringCchCopyW(g.nid.szTip, ARRAYSIZE(g.nid.szTip), L"KeyBeep");
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g.nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Open Settings");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    bool autoStartOn = IsAutoStartEnabled();
    bool isInstalled = IsInstalledInstance();

    // Autostart toggle (only meaningful if installed)
    UINT autoFlags = MF_STRING | (autoStartOn ? MF_CHECKED : MF_UNCHECKED);
    if (!isInstalled) autoFlags |= MF_GRAYED;
    AppendMenuW(hMenu, autoFlags, ID_TRAY_AUTOSTART,
        autoStartOn ? L"Autostart: Enabled" : L"Autostart: Disabled");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    if (!isInstalled) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_INSTALL,
            L"Install (add to autostart)");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, ID_TRAY_INSTALL, L"Already installed");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_UNINSTALL, L"Uninstall");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ---- Create main window UI ----
void CreateControls(HWND hwnd) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Title
    HWND lbl = CreateWindowExW(0, L"STATIC", L"KeyBeep - keyboard press sound",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 10, 360, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Current hotkey display
    g.hwndStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 40, 360, 24, hwnd, (HMENU)ID_STATIC_HOTKEY, NULL, NULL);
    SendMessageW(g.hwndStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Modifier checkboxes
    HWND lbl2 = CreateWindowExW(0, L"STATIC", L"Modifiers (optional):",
        WS_CHILD | WS_VISIBLE,
        10, 76, 180, 18, hwnd, NULL, NULL, NULL);
    SendMessageW(lbl2, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndCheckCtrl = CreateWindowExW(0, L"BUTTON", L"Ctrl",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 96, 70, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g.hwndCheckCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndCheckAlt = CreateWindowExW(0, L"BUTTON", L"Alt",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        85, 96, 70, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g.hwndCheckAlt, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndCheckShift = CreateWindowExW(0, L"BUTTON", L"Shift",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        160, 96, 70, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g.hwndCheckShift, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndCheckWin = CreateWindowExW(0, L"BUTTON", L"Win",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        235, 96, 70, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g.hwndCheckWin, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Buttons: Set key, Clear
    g.hwndBtnSet = CreateWindowExW(0, L"BUTTON", L"Set Key",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 126, 170, 28, hwnd, (HMENU)ID_BTN_SET_KEY, NULL, NULL);
    SendMessageW(g.hwndBtnSet, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndBtnClear = CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        190, 126, 90, 28, hwnd, (HMENU)ID_BTN_CLEAR, NULL, NULL);
    SendMessageW(g.hwndBtnClear, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Sound selection
    HWND lbl3 = CreateWindowExW(0, L"STATIC", L"Sound type:",
        WS_CHILD | WS_VISIBLE,
        10, 168, 80, 18, hwnd, NULL, NULL, NULL);
    SendMessageW(lbl3, WM_SETFONT, (WPARAM)hFont, TRUE);

    g.hwndCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        95, 164, 275, 200, hwnd, (HMENU)ID_COMBO_SOUND, NULL, NULL);
    SendMessageW(g.hwndCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    for (int i = 0; i < SOUND_COUNT; i++)
        SendMessageW(g.hwndCombo, CB_ADDSTRING, 0, (LPARAM)SOUND_NAMES[i]);
    SendMessageW(g.hwndCombo, CB_SETCURSEL, (WPARAM)g.soundType, 0);

    // Test sound button
    g.hwndBtnTest = CreateWindowExW(0, L"BUTTON", L"Test Sound",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 200, 120, 28, hwnd, (HMENU)ID_BTN_TEST, NULL, NULL);
    SendMessageW(g.hwndBtnTest, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Enable checkbox
    HWND chkEnable = CreateWindowExW(0, L"BUTTON", L"Enabled",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        250, 204, 120, 20, hwnd, (HMENU)5001, NULL, NULL);
    SendMessageW(chkEnable, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(chkEnable, BM_SETCHECK, g.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    // Separator line (static)
    CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        10, 240, 360, 2, hwnd, NULL, NULL, NULL);

    // Info label
    HWND info = CreateWindowExW(0, L"STATIC",
        L"App runs in tray. Press the assigned key to hear the sound.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 248, 360, 32, hwnd, NULL, NULL, NULL);
    SendMessageW(info, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Sync checkboxes from loaded state
    SendMessageW(g.hwndCheckCtrl,  BM_SETCHECK, g.needCtrl  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.hwndCheckAlt,   BM_SETCHECK, g.needAlt   ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.hwndCheckShift, BM_SETCHECK, g.needShift ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.hwndCheckWin,   BM_SETCHECK, g.needWin   ? BST_CHECKED : BST_UNCHECKED, 0);

    UpdateHotkeyLabel();
}

// ---- Window procedure ----
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g.hwndMain = hwnd;
        AddTrayIcon(hwnd);
        CreateControls(hwnd);
        InitMidiOut();
        return 0;

    case WM_USER + 10:  // play sound (posted from hook)
        PlayConfiguredSound();
        return 0;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == ID_BTN_SET_KEY) {
            g.waitingForKey = true;
            SetWindowTextW(g.hwndBtnSet, L"Press a key...");
            // Auto-cancel after 5 seconds
            SetTimer(hwnd, TIMER_WAITING_KEY, 5000, NULL);
        }
        else if (id == ID_BTN_CLEAR) {
            g.triggerVK = 0;
            g.needCtrl = g.needAlt = g.needShift = g.needWin = false;
            SendMessageW(g.hwndCheckCtrl,  BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(g.hwndCheckAlt,   BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(g.hwndCheckShift, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(g.hwndCheckWin,   BM_SETCHECK, BST_UNCHECKED, 0);
            UpdateHotkeyLabel();
            SaveSettings();
        }
        else if (id == ID_BTN_TEST) {
            PlayConfiguredSound();
        }
        else if (id == ID_COMBO_SOUND && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g.hwndCombo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) g.soundType = (SoundType)sel;
            SaveSettings();
        }
        else if (id == 5001) {  // Enable checkbox
            g.enabled = (SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveSettings();
        }
        else if (id == ID_TRAY_SHOW) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        else if (id == ID_TRAY_AUTOSTART) {
            bool cur = IsAutoStartEnabled();
            SetAutoStart(!cur);
        }
        else if (id == ID_TRAY_INSTALL) {
            if (SelfInstall()) {
                // Launch installed copy and exit current instance
                wchar_t installPath[MAX_PATH] = {};
                GetInstallPath(installPath, MAX_PATH);
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                if (CreateProcessW(installPath, NULL, NULL, NULL, FALSE,
                    0, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
                MessageBoxW(hwnd,
                    L"Installed successfully!\n"
                    L"Program copied to:\n"
                    L"%LOCALAPPDATA%\\KeyBeep\\KeyBeep.exe\n\n"
                    L"Added to Windows autostart.",
                    APP_NAME, MB_ICONINFORMATION);
                DestroyWindow(hwnd);
            }
        }
        else if (id == ID_TRAY_UNINSTALL) {
            int res = MessageBoxW(hwnd,
                L"Uninstall KeyBeep?\n"
                L"Files and autostart entry will be removed.",
                APP_NAME, MB_ICONQUESTION | MB_YESNO);
            if (res == IDYES) {
                SelfUninstall();
                DestroyWindow(hwnd);
            }
        }
        else if (id == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_WAITING_KEY && g.waitingForKey) {
            g.waitingForKey = false;
            SetWindowTextW(g.hwndBtnSet, L"Set Key");
            KillTimer(hwnd, TIMER_WAITING_KEY);
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        // Minimize to tray instead of closing
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g.hKeyboardHook) UnhookWindowsHookEx(g.hKeyboardHook);
        CloseMidiOut();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---- WinMain ----
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {

    // ---- Handle /uninstall command (called from Add/Remove Programs) ----
    LPWSTR cmd = GetCommandLineW();
    if (wcsstr(cmd, L"/uninstall") || wcsstr(cmd, L"-uninstall")) {
        int r = MessageBoxW(NULL,
            L"Uninstall KeyBeep?\n\n"
            L"The program, shortcuts, and autostart entry will be removed.",
            L"Uninstall KeyBeep", MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES) {
            // Terminate any running instance
            HWND hw = FindWindowW(L"KeyBeepWnd", NULL);
            if (hw) {
                DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); Sleep(700); }
            }
            // Remove autostart
            SetAutoStart(false);
            // Remove uninstall registry key
            RegDeleteKeyW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KeyBeep");
            // Remove app settings
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\KeyBeep");
            // Remove Start Menu shortcut
            wchar_t path[MAX_PATH] = {}, lnk[MAX_PATH] = {};
            if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path) == S_OK) {
                StringCchPrintfW(lnk, MAX_PATH, L"%s\\KeyBeep.lnk", path);
                DeleteFileW(lnk);
            }
            // Remove Desktop shortcut
            if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path) == S_OK) {
                StringCchPrintfW(lnk, MAX_PATH, L"%s\\KeyBeep.lnk", path);
                DeleteFileW(lnk);
            }
            // Schedule self-deletion of exe and folder
            SelfUninstall();
            MessageBoxW(NULL, L"KeyBeep has been uninstalled.",
                L"Uninstall", MB_ICONINFORMATION);
        }
        return 0;
    }

    // Prevent multiple instances
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"KeyBeepMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"KeyBeep is already running!", L"KeyBeep", MB_ICONINFORMATION);
        return 0;
    }

    // Init common controls
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    // Default state
    ZeroMemory(&g, sizeof(g));
    g.triggerVK = 0;
    g.soundType = SOUND_BEEP_MID;
    g.enabled   = true;

    LoadSettings();

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KeyBeepWnd";
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MAIN), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Create window
    HWND hwnd = CreateWindowExW(0, L"KeyBeepWnd", L"KeyBeep",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 310,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Install global keyboard hook
    g.hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                         hInstance, 0);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
