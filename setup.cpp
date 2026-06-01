// KeyBeep Setup - Windows Installer
// Pure WinAPI + built-in COM (shell32), no external dependencies
// Embeds KeyBeep.exe as RCDATA resource, extracts and installs on run

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <strsafe.h>
#include "setup_resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ---- Constants ----
static const wchar_t* APP_NAME     = L"KeyBeep";
static const wchar_t* APP_VER      = L"1.0";
static const wchar_t* EXE_NAME     = L"KeyBeep.exe";
static const wchar_t* UNINST_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KeyBeep";
static const wchar_t* AUTORUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static const int WND_W = 500, WND_H = 430, HDR_H = 70;
static const COLORREF CLR_HDR = RGB(0, 120, 215);

// ---- Control IDs ----
enum {
    ID_NEXT = 101, ID_BACK, ID_CANCEL,
    ID_EDITPATH = 201, ID_BROWSE,
    ID_CHK_AUTO, ID_CHK_STARTMENU, ID_CHK_DESKTOP,
    ID_PROGRESS = 301, ID_STATUS, ID_LOG,
    ID_CHK_LAUNCH = 401
};

// ---- Pages ----
enum Page { PG_WELCOME, PG_OPTIONS, PG_INSTALL, PG_DONE };

// ---- Content control tracker (for page cleanup) ----
static HWND  gContent[48];
static int   gContentN = 0;

static HWND Track(HWND h) {
    if (h && gContentN < 48) gContent[gContentN++] = h;
    return h;
}

// ---- State ----
static struct {
    HWND hwnd;
    Page page;
    wchar_t dir[MAX_PATH];
    bool optAuto, optStart, optDesk, optLaunch;
    bool success;
    bool installFailed; // install finished with failure, Cancel = Close
    // fonts & brushes
    HFONT fTitle, fSub, fNormal, fBold;
    HBRUSH brBlue, brBtnFace;
    // named content controls
    HWND hEditPath, hBrowse;
    HWND hChkAuto, hChkStart, hChkDesk, hChkLaunch;
    HWND hProgress, hStatus, hLog;
    // nav buttons (always present)
    HWND hNext, hBack, hCancel;
} S;

// ---- Font helper ----
static HFONT MakeFont(int pts, bool bold, const wchar_t* face = L"Segoe UI") {
    HDC dc = GetDC(NULL);
    int h = -MulDiv(pts, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    return CreateFontW(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// ---- Control factories ----
static HWND MkLabel(const wchar_t* txt, int x, int y, int w, int h, bool bold = false) {
    HWND c = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, S.hwnd, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(bold ? S.fBold : S.fNormal), FALSE);
    return Track(c);
}
static HWND MkCheck(const wchar_t* txt, int id, int x, int y, int w, bool chk) {
    HWND c = CreateWindowExW(0, L"BUTTON", txt,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        x, y, w, 20, S.hwnd, (HMENU)(UINT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)S.fNormal, FALSE);
    SendMessageW(c, BM_SETCHECK, chk ? BST_CHECKED : BST_UNCHECKED, 0);
    return Track(c);
}
static HWND MkButton(const wchar_t* txt, int id, int x, int y, int w, int h, bool def = false) {
    DWORD s = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : 0);
    HWND c = CreateWindowExW(0, L"BUTTON", txt, s,
        x, y, w, h, S.hwnd, (HMENU)(UINT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)S.fNormal, FALSE);
    return c; // nav buttons not tracked (not in content area)
}

// ---- Clear current page content ----
static void ClearContent() {
    for (int i = 0; i < gContentN; i++) if (gContent[i]) DestroyWindow(gContent[i]);
    gContentN = 0;
    S.hEditPath = S.hBrowse = S.hChkAuto = S.hChkStart
                = S.hChkDesk = S.hChkLaunch = S.hProgress = S.hStatus = S.hLog = NULL;
}

// ---- Status/progress helpers (called from install thread) ----
static void SetStatus(const wchar_t* msg) {
    if (S.hStatus) { SetWindowTextW(S.hStatus, msg); UpdateWindow(S.hStatus); }
}
static void SetProg(int v) {
    if (S.hProgress) SendMessageW(S.hProgress, PBM_SETPOS, v, 0);
}
// AddLog: safe to call from any thread — posts heap string, WndProc frees it.
static void AddLog(const wchar_t* msg) {
    wchar_t* copy = _wcsdup(msg);
    if (copy) PostMessageW(S.hwnd, WM_USER + 21, 0, (LPARAM)copy);
}

// ---- Extract embedded payload to file ----
static bool ExtractPayload(const wchar_t* dest) {
    wchar_t buf[256] = {};
    HMODULE mod = GetModuleHandleW(NULL);
    HRSRC   hr  = FindResourceW(mod, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!hr) {
        StringCchPrintfW(buf, 256, L"  ERROR: Payload resource not found in setup.exe (err=%lu)", GetLastError());
        AddLog(buf); return false;
    }
    HGLOBAL hg = LoadResource(mod, hr);
    if (!hg) { AddLog(L"  ERROR: LoadResource failed."); return false; }
    void*  p  = LockResource(hg);
    DWORD  sz = SizeofResource(mod, hr);
    StringCchPrintfW(buf, 256, L"  Payload size: %lu bytes", sz);
    AddLog(buf);
    HANDLE f  = CreateFileW(dest, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        StringCchPrintfW(buf, 256, L"  ERROR: Cannot create file (err=%lu)", GetLastError());
        AddLog(buf); return false;
    }
    DWORD wr = 0;
    WriteFile(f, p, sz, &wr, NULL);
    CloseHandle(f);
    if (wr != sz) {
        StringCchPrintfW(buf, 256, L"  ERROR: Write incomplete (%lu/%lu bytes)", wr, sz);
        AddLog(buf); return false;
    }
    return true;
}

// FOLDERID_Desktop GUID (KnownFolders.h, inlined to avoid extra header)
static const GUID KB_FOLDERID_Desktop =
    {0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0,0x29,0x7F,0xE9,0x9A,0x87,0xC6,0x41}};

// ---- Create .lnk shortcut using built-in COM ----
// COM must already be initialized on the calling thread.
static bool MakeShortcut(const wchar_t* target, const wchar_t* lnkPath) {
    bool ok = false;
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void**)&psl);
    if (SUCCEEDED(hr)) {
        psl->SetPath(target);
        psl->SetIconLocation(target, 0);
        psl->SetDescription(L"KeyBeep - keyboard press sound notifier");
        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(lnkPath, TRUE);
            ok = SUCCEEDED(hr);
            ppf->Release();
        }
        psl->Release();
    }
    if (!ok) {
        wchar_t logbuf[64];
        StringCchPrintfW(logbuf, 64, L"  Shortcut FAILED hr=0x%08X", (DWORD)hr);
        AddLog(logbuf);
    }
    return ok;
}

// ---- Install worker thread ----
static DWORD WINAPI InstallThread(LPVOID) {
    // COM initialized once for the whole thread (IShellLink needs STA).
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    wchar_t exePath[MAX_PATH] = {};
    StringCchPrintfW(exePath, MAX_PATH, L"%s\\%s", S.dir, EXE_NAME);

    SetStatus(L"Creating install directory..."); SetProg(8);
    AddLog(L"Creating install directory...");
    if (!CreateDirectoryW(S.dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        AddLog(L"ERROR: Cannot create directory.");
        SetStatus(L"ERROR: Cannot create directory.");
        S.success = false; CoUninitialize(); PostMessageW(S.hwnd, WM_USER + 20, 0, 0); return 0;
    }

    // Stop running instance if any, wait for process exit
    SetStatus(L"Stopping running instance..."); SetProg(12);
    HWND running = FindWindowW(L"KeyBeepWnd", NULL);
    if (running) {
        AddLog(L"Stopping running KeyBeep...");
        DWORD pid = 0; GetWindowThreadProcessId(running, &pid);
        HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (hp) {
            PostMessageW(running, WM_CLOSE, 0, 0);           // ask nicely first
            if (WaitForSingleObject(hp, 2000) != WAIT_OBJECT_0)
                TerminateProcess(hp, 0);                      // force if needed
            WaitForSingleObject(hp, 3000);
            CloseHandle(hp);
        }
        AddLog(L"  Stopped.");
    }

    // Remove old exe so we can overwrite even if it was locked
    if (GetFileAttributesW(exePath) != INVALID_FILE_ATTRIBUTES) {
        AddLog(L"Removing old files...");
        DeleteFileW(exePath);
    }

    SetStatus(L"Copying files..."); SetProg(25);
    AddLog(L"Copying files...");
    if (!ExtractPayload(exePath)) {
        SetStatus(L"ERROR: Cannot write file.");
        S.success = false; CoUninitialize(); PostMessageW(S.hwnd, WM_USER + 20, 0, 0); return 0;
    }
    AddLog(L"  File copied OK.");

    if (S.optAuto) {
        SetStatus(L"Registering autostart..."); SetProg(45);
        AddLog(L"Registering autostart...");
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
            wchar_t q[MAX_PATH + 4] = {};
            StringCchPrintfW(q, MAX_PATH + 4, L"\"%s\"", exePath);
            RegSetValueExW(hk, APP_NAME, 0, REG_SZ,
                (BYTE*)q, (DWORD)((wcslen(q) + 1) * sizeof(wchar_t)));
            RegCloseKey(hk);
            AddLog(L"  Autostart registered OK.");
        } else {
            AddLog(L"  WARNING: Could not open Run key.");
        }
    }

    if (S.optStart) {
        SetStatus(L"Creating Start Menu shortcut..."); SetProg(60);
        AddLog(L"Creating Start Menu shortcut...");
        wchar_t smDir[MAX_PATH] = {}, lnk[MAX_PATH] = {};
        HRESULT hr2 = SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, smDir);
        if (hr2 == S_OK) {
            StringCchPrintfW(lnk, MAX_PATH, L"%s\\%s.lnk", smDir, APP_NAME);
            AddLog(lnk);
            bool ok = MakeShortcut(exePath, lnk);
            AddLog(ok ? L"  Start Menu shortcut: OK" : L"  Start Menu shortcut: FAILED");
        } else {
            wchar_t logbuf[64]; StringCchPrintfW(logbuf,64,L"  SHGetFolderPath failed hr=0x%08X",(DWORD)hr2);
            AddLog(logbuf);
        }
    }

    if (S.optDesk) {
        SetStatus(L"Creating Desktop shortcut..."); SetProg(72);
        AddLog(L"Creating Desktop shortcut...");
        // Use SHGetKnownFolderPath — correctly resolves OneDrive-redirected Desktop
        PWSTR pDesk = nullptr;
        HRESULT hr2 = SHGetKnownFolderPath(KB_FOLDERID_Desktop, 0, NULL, &pDesk);
        if (SUCCEEDED(hr2) && pDesk) {
            wchar_t lnk[MAX_PATH] = {};
            StringCchPrintfW(lnk, MAX_PATH, L"%s\\%s.lnk", pDesk, APP_NAME);
            AddLog(lnk);
            bool ok = MakeShortcut(exePath, lnk);
            AddLog(ok ? L"  Desktop shortcut: OK" : L"  Desktop shortcut: FAILED");
            CoTaskMemFree(pDesk);
        } else {
            wchar_t logbuf[64]; StringCchPrintfW(logbuf,64,L"  SHGetKnownFolderPath failed hr=0x%08X",(DWORD)hr2);
            AddLog(logbuf);
        }
    }

    SetStatus(L"Registering uninstaller..."); SetProg(85);
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        auto ss = [&](const wchar_t* n, const wchar_t* v) {
            RegSetValueExW(hk, n, 0, REG_SZ, (BYTE*)v,
                (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
        };
        auto sd = [&](const wchar_t* n, DWORD v) {
            RegSetValueExW(hk, n, 0, REG_DWORD, (BYTE*)&v, sizeof(v));
        };
        wchar_t ucmd[MAX_PATH + 20] = {};
        StringCchPrintfW(ucmd, MAX_PATH + 20, L"\"%s\" /uninstall", exePath);
        ss(L"DisplayName",          APP_NAME);
        ss(L"DisplayVersion",       APP_VER);
        ss(L"Publisher",            L"KeyBeep");
        ss(L"InstallLocation",      S.dir);
        ss(L"DisplayIcon",          exePath);
        ss(L"UninstallString",      ucmd);
        ss(L"QuietUninstallString", ucmd);
        sd(L"NoModify",  1);
        sd(L"NoRepair",  1);
        sd(L"EstimatedSize", 200);
        RegCloseKey(hk);
    }

    SetProg(100); SetStatus(L"Installation complete!");
    S.success = true;
    CoUninitialize();
    PostMessageW(S.hwnd, WM_USER + 20, 0, 0);
    return 0;
}

// ---- Page builder ----
static void ShowPage(Page pg) {
    ClearContent();
    S.page = pg;

    // Update nav buttons
    switch (pg) {
    case PG_WELCOME:
        SetWindowTextW(S.hNext, L"Next >");
        EnableWindow(S.hNext, TRUE);
        ShowWindow(S.hBack,   SW_HIDE);
        ShowWindow(S.hCancel, SW_SHOW);
        ShowWindow(S.hNext,   SW_SHOW);
        break;
    case PG_OPTIONS:
        SetWindowTextW(S.hNext, L"Install");
        EnableWindow(S.hNext, TRUE);
        ShowWindow(S.hBack,   SW_SHOW);
        ShowWindow(S.hCancel, SW_SHOW);
        ShowWindow(S.hNext,   SW_SHOW);
        break;
    case PG_INSTALL:
        ShowWindow(S.hNext,   SW_HIDE);
        ShowWindow(S.hBack,   SW_HIDE);
        ShowWindow(S.hCancel, SW_HIDE);
        break;
    case PG_DONE:
        SetWindowTextW(S.hNext, L"Finish");
        EnableWindow(S.hNext, TRUE);
        ShowWindow(S.hBack,   SW_HIDE);
        ShowWindow(S.hCancel, SW_HIDE);
        ShowWindow(S.hNext,   SW_SHOW);
        break;
    }

    int cy = HDR_H + 18; // content start Y

    if (pg == PG_WELCOME) {
        MkLabel(L"This wizard will install KeyBeep on your computer.", 30, cy, 440, 18, true); cy += 30;
        MkLabel(L"KeyBeep plays a confirmation sound when you press a", 30, cy, 440, 18); cy += 20;
        MkLabel(L"configured key or key combination - ideal for confirming", 30, cy, 440, 18); cy += 20;
        MkLabel(L"keyboard language layout switches.",                        30, cy, 440, 18); cy += 34;
        MkLabel(L"Default install location:", 30, cy, 180, 18, true); cy += 22;
        MkLabel(L"%LOCALAPPDATA%\\KeyBeep\\",  30, cy, 440, 18); cy += 30;
        MkLabel(L"Click Next to choose installation options.",  30, cy, 440, 18);
    }
    else if (pg == PG_OPTIONS) {
        MkLabel(L"Install location:", 30, cy, 140, 18); cy += 20;
        S.hEditPath = Track(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", S.dir,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            30, cy, 355, 22, S.hwnd, (HMENU)ID_EDITPATH, NULL, NULL));
        SendMessageW(S.hEditPath, WM_SETFONT, (WPARAM)S.fNormal, FALSE);
        S.hBrowse = Track(MkButton(L"Browse...", ID_BROWSE, 392, cy, 78, 22));
        cy += 36;
        MkLabel(L"Options:", 30, cy, 80, 18, true); cy += 22;
        S.hChkAuto  = MkCheck(L"Add to Windows autostart (start with Windows)",
                               ID_CHK_AUTO,      44, cy, 420, S.optAuto);  cy += 26;
        S.hChkStart = MkCheck(L"Create Start Menu shortcut",
                               ID_CHK_STARTMENU, 44, cy, 320, S.optStart); cy += 26;
        S.hChkDesk  = MkCheck(L"Create Desktop shortcut",
                               ID_CHK_DESKTOP,   44, cy, 320, S.optDesk);
    }
    else if (pg == PG_INSTALL) {
        MkLabel(L"Installing KeyBeep, please wait...", 30, cy, 440, 20, true); cy += 28;
        S.hProgress = Track(CreateWindowExW(0, PROGRESS_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            30, cy, 440, 22, S.hwnd, (HMENU)ID_PROGRESS, NULL, NULL));
        SendMessageW(S.hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(S.hProgress, PBM_SETPOS, 0, 0);
        cy += 30;
        // Multi-line log (read-only edit, auto-scroll to bottom)
        S.hLog = Track(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            30, cy, 440, 150, S.hwnd, (HMENU)ID_LOG, NULL, NULL));
        SendMessageW(S.hLog, WM_SETFONT, (WPARAM)S.fNormal, FALSE);
        cy += 158;
        S.hStatus = Track(CreateWindowExW(0, L"STATIC", L"Starting...",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            30, cy, 440, 18, S.hwnd, (HMENU)ID_STATUS, NULL, NULL));
        SendMessageW(S.hStatus, WM_SETFONT, (WPARAM)S.fNormal, FALSE);
        // Launch install worker thread
        CloseHandle(CreateThread(NULL, 0, InstallThread, NULL, 0, NULL));
    }
    else if (pg == PG_DONE) {
        if (S.success) {
            MkLabel(L"KeyBeep was installed successfully!", 30, cy, 440, 20, true); cy += 30;
            MkLabel(L"Configure your hotkey by right-clicking the tray icon", 30, cy, 440, 18); cy += 22;
            MkLabel(L"next to the clock in the taskbar.",                      30, cy, 440, 18); cy += 36;
            S.hChkLaunch = MkCheck(L"Launch KeyBeep now",
                                   ID_CHK_LAUNCH, 30, cy, 280, true);
            S.optLaunch = true;
        } else {
            MkLabel(L"Installation failed.", 30, cy, 440, 20, true); cy += 30;
            MkLabel(L"Check that no other instance is running and retry.", 30, cy, 440, 18);
        }
    }

    InvalidateRect(S.hwnd, NULL, TRUE);
    UpdateWindow(S.hwnd);
}

// ---- Window procedure ----
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        S.hwnd = hwnd;
        // Bottom nav buttons (permanent)
        S.hNext   = MkButton(L"Next >",  ID_NEXT,   WND_W-88-10,             WND_H-38, 88, 26, true);
        S.hBack   = MkButton(L"< Back",  ID_BACK,   WND_W-88-10-88-6,        WND_H-38, 88, 26);
        S.hCancel = MkButton(L"Cancel",  ID_CANCEL, WND_W-88-10-88-6-88-16,  WND_H-38, 88, 26);
        ShowPage(PG_WELCOME);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        // Blue header
        RECT hdr = { 0, 0, WND_W, HDR_H };
        FillRect(dc, &hdr, S.brBlue);

        // Header text
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        SelectObject(dc, S.fTitle);
        RECT rt = { 18, 8, WND_W - 18, HDR_H / 2 + 6 };
        DrawTextW(dc, L"KeyBeep 1.0", -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, S.fSub);
        static const wchar_t* subs[] = {
            L"Welcome to the Setup Wizard",
            L"Choose installation options",
            L"Installing...",
            S.success ? L"Installation Complete" : L"Installation Failed"
        };
        RECT rs = { 18, HDR_H / 2 + 6, WND_W - 18, HDR_H - 6 };
        DrawTextW(dc, subs[S.page], -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Separator above buttons
        RECT sep = { 0, WND_H - 46, WND_W, WND_H - 45 };
        FillRect(dc, &sep, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // Make static controls transparent over the button-face background
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)S.brBtnFace;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case ID_CANCEL:
            if (S.installFailed) { DestroyWindow(hwnd); break; }
            if (S.page == PG_INSTALL) break; // can't cancel mid-install
            if (MessageBoxW(hwnd, L"Cancel the installation?",
                            L"Setup", MB_YESNO | MB_ICONQUESTION) == IDYES)
                DestroyWindow(hwnd);
            break;

        case ID_BACK:
            if (S.page == PG_OPTIONS) ShowPage(PG_WELCOME);
            break;

        case ID_NEXT:
            if (S.page == PG_WELCOME) {
                ShowPage(PG_OPTIONS);
            }
            else if (S.page == PG_OPTIONS) {
                GetWindowTextW(S.hEditPath, S.dir, MAX_PATH);
                if (S.dir[0] == 0) {
                    MessageBoxW(hwnd, L"Please specify an install location.", L"Setup", MB_ICONWARNING);
                    break;
                }
                S.optAuto  = SendMessageW(S.hChkAuto,  BM_GETCHECK, 0, 0) == BST_CHECKED;
                S.optStart = SendMessageW(S.hChkStart, BM_GETCHECK, 0, 0) == BST_CHECKED;
                S.optDesk  = SendMessageW(S.hChkDesk,  BM_GETCHECK, 0, 0) == BST_CHECKED;
                ShowPage(PG_INSTALL);
            }
            else if (S.page == PG_DONE) {
                if (S.hChkLaunch)
                    S.optLaunch = SendMessageW(S.hChkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED;
                DestroyWindow(hwnd);
            }
            break;

        case ID_BROWSE: {
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Select install folder:";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t base[MAX_PATH] = {}, full[MAX_PATH] = {};
                SHGetPathFromIDListW(pidl, base);
                StringCchPrintfW(full, MAX_PATH, L"%s\\%s", base, APP_NAME);
                SetWindowTextW(S.hEditPath, full);
                StringCchCopyW(S.dir, MAX_PATH, full);
                CoTaskMemFree(pidl);
            }
            break;
        }

        case ID_CHK_LAUNCH:
            S.optLaunch = (SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        }
        return 0;
    }

    case WM_USER + 20: // install thread finished
        if (S.success) {
            ShowPage(PG_DONE);
        } else {
            // Stay on PG_INSTALL so user can read the log.
            S.installFailed = true;
            SetWindowTextW(S.hStatus, L"\u274C Installation FAILED. See log above.");
            UpdateWindow(S.hStatus);
            SetWindowTextW(S.hCancel, L"Close");
            ShowWindow(S.hCancel, SW_SHOW);
            EnableWindow(S.hCancel, TRUE);
        }
        return 0;

    case WM_USER + 21: { // AddLog message from install thread
        wchar_t* text = (wchar_t*)lp;
        if (text && S.hLog) {
            int len = GetWindowTextLengthW(S.hLog);
            SendMessageW(S.hLog, EM_SETSEL, len, len);
            SendMessageW(S.hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
            SendMessageW(S.hLog, EM_SETSEL, -1, -1);
            SendMessageW(S.hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
            SendMessageW(S.hLog, EM_SCROLLCARET, 0, 0);
            // Force repaint immediately so user sees the line before next message
            UpdateWindow(S.hLog);
        }
        free(text);
        return 0;
    }

    case WM_DESTROY:
        // Launch app if requested
        if (S.success && S.optLaunch) {
            wchar_t exePath[MAX_PATH] = {};
            StringCchPrintfW(exePath, MAX_PATH, L"%s\\%s", S.dir, EXE_NAME);
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
        DeleteObject(S.fTitle);
        DeleteObject(S.fSub);
        DeleteObject(S.fBold);
        DeleteObject(S.brBlue);
        PostQuitMessage(S.success ? 0 : 1);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_RETURN && S.page != PG_INSTALL)
            SendMessageW(hwnd, WM_COMMAND, ID_NEXT, 0);
        if (wp == VK_ESCAPE && S.page != PG_INSTALL)
            SendMessageW(hwnd, WM_COMMAND, ID_CANCEL, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Entry point ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Prevent duplicate setup
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"KeyBeepSetupMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Setup is already running.", L"Setup", MB_ICONINFORMATION);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    // Initialize state
    ZeroMemory(&S, sizeof(S));
    S.optAuto   = true;
    S.optStart  = true;
    S.optDesk   = false;
    S.optLaunch = true;
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\KeyBeep", S.dir, MAX_PATH);

    // Fonts
    S.fTitle  = MakeFont(16, true);
    S.fSub    = MakeFont(9,  false);
    S.fNormal = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    S.fBold   = MakeFont(9,  true);

    // Brushes
    S.brBlue    = CreateSolidBrush(CLR_HDR);
    S.brBtnFace = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

    // Compute exact window size so client area == WND_W x WND_H
    DWORD wStyle  = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD wExStyle = WS_EX_DLGMODALFRAME;
    RECT rc = { 0, 0, WND_W, WND_H };
    AdjustWindowRectEx(&rc, wStyle, FALSE, wExStyle);
    int winW = rc.right  - rc.left;
    int winH = rc.bottom - rc.top;

    // Register window class
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KeyBeepSetup";
    wc.hIcon   = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAIN));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MAIN), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Center on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(wExStyle, L"KeyBeepSetup",
        L"KeyBeep 1.0 Setup",
        wStyle,
        (sw - winW) / 2, (sh - winH) / 2, winW, winH,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
