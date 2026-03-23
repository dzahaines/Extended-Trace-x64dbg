#include "dialog.h"
#include "tracer.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

#define IDC_EDT_OUTPUT     1001
#define IDC_BTN_BROWSE     1002
#define IDC_CHK_SKIPSYS    1003
#define IDC_LV_MODULES     1004
#define IDC_BTN_REFRESH    1005
#define IDC_BTN_START      1006
#define IDC_BTN_STOP       1007
#define IDC_LBL_STATUS     1008
#define IDC_CHK_DUMP       1009
#define IDC_BTN_SELALL     1010
#define IDC_BTN_DESALL     1011
#define IDC_BTN_REVSEL     1012
#define IDC_CMB_SKIPMODE   1013
#define IDC_CHK_LOGCALLS   1014
#define IDC_CHK_FINISH_AT_STOP 1015

static HWND hDialog = nullptr;
static HWND hEditOutput = nullptr;
static HWND hChkSkipSys = nullptr;
static HWND hCmbSkipMode = nullptr;
static HWND hChkLogCalls = nullptr;
static HWND hChkDump = nullptr;
static HWND hChkFinishAtStop = nullptr;
static HWND hListModules = nullptr;
static HWND hBtnStart = nullptr;
static HWND hBtnStop = nullptr;
static HWND hBtnRefresh = nullptr;
static HWND hLblTime = nullptr;
static HWND hLblInstructions = nullptr;
static HWND hLblStatus = nullptr;
static HWND hSeparator = nullptr;
static UINT_PTR timerID = 0;
static bool g_populatingList = false;

static const wchar_t* DIALOG_CLASS = L"ExtendedTraceDialog";

static std::string GetIniPath()
{
    char dir[MAX_PATH] = "";
    HMODULE hMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetIniPath, &hMod);
    GetModuleFileNameA(hMod, dir, MAX_PATH);

    char* slash = strrchr(dir, '\\');
    if (slash)
        *(slash + 1) = '\0';
    else
        dir[0] = '\0';

    return std::string(dir) + "ExtendedTrace.ini";
}

static std::string BuildModuleSetKey()
{
    if (!hListModules)
        return "";

    std::vector<std::string> names;
    int count = ListView_GetItemCount(hListModules);
    for (int i = 0; i < count; i++)
    {
        char text[MAX_MODULE_SIZE] = "";
        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = text;
        lvi.cchTextMax = sizeof(text);
        SendMessageA(hListModules, LVM_GETITEMA, 0, (LPARAM)&lvi);

        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        names.push_back(lower);
    }
    std::sort(names.begin(), names.end());

    std::string key;
    for (const auto& n : names)
    {
        if (!key.empty())
            key += ";";
        key += n;
    }
    return key;
}

static std::string BuildCheckedModulesString()
{
    std::string csv;
    if (!hListModules)
        return csv;

    int count = ListView_GetItemCount(hListModules);
    for (int i = 0; i < count; i++)
    {
        if (!ListView_GetCheckState(hListModules, i))
            continue;

        char text[MAX_MODULE_SIZE] = "";
        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = text;
        lvi.cchTextMax = sizeof(text);
        SendMessageA(hListModules, LVM_GETITEMA, 0, (LPARAM)&lvi);

        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        if (!csv.empty())
            csv += ";";
        csv += lower;
    }
    return csv;
}

static void SaveSettings()
{
    std::string iniPath = GetIniPath();
    const char* ini = iniPath.c_str();
    const char* section = "Settings";

    char path[MAX_PATH] = "";
    if (hEditOutput)
        GetWindowTextA(hEditOutput, path, MAX_PATH);
    WritePrivateProfileStringA(section, "OutputFolder", path, ini);

    bool skipSys = hChkSkipSys && (SendMessage(hChkSkipSys, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool logCalls = hChkLogCalls && (SendMessage(hChkLogCalls, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool dump = hChkDump && (SendMessage(hChkDump, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool finishAtFirstStop = hChkFinishAtStop && (SendMessage(hChkFinishAtStop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int skipModeIdx = hCmbSkipMode ? (int)SendMessage(hCmbSkipMode, CB_GETCURSEL, 0, 0) : 0;
    if (skipModeIdx < 0) skipModeIdx = 0;
    WritePrivateProfileStringA(section, "SkipSystemModules", skipSys ? "1" : "0", ini);
    WritePrivateProfileStringA(section, "LogSkippedCalls", logCalls ? "1" : "0", ini);
    WritePrivateProfileStringA(section, "DumpTracedModules", dump ? "1" : "0", ini);
    WritePrivateProfileStringA(section, "FinishAtFirstStop", finishAtFirstStop ? "1" : "0", ini);
    WritePrivateProfileStringA(section, "SkipMode", std::to_string(skipModeIdx).c_str(), ini);

    std::string moduleKey = BuildModuleSetKey();
    if (moduleKey.empty())
        return;

    std::string checkedStr = BuildCheckedModulesString();

    int profileCount = GetPrivateProfileIntA("ModuleProfiles", "Count", 0, ini);

    int existingIdx = -1;
    for (int i = 0; i < profileCount; i++)
    {
        char secName[64];
        snprintf(secName, sizeof(secName), "ModuleSet_%d", i);

        char keyBuf[8192] = "";
        GetPrivateProfileStringA(secName, "Keys", "", keyBuf, sizeof(keyBuf), ini);

        if (moduleKey == keyBuf)
        {
            existingIdx = i;
            break;
        }
    }

    int idx = existingIdx;
    if (idx < 0)
    {
        idx = profileCount;
        WritePrivateProfileStringA("ModuleProfiles", "Count",
            std::to_string(profileCount + 1).c_str(), ini);
    }

    char secName[64];
    snprintf(secName, sizeof(secName), "ModuleSet_%d", idx);
    WritePrivateProfileStringA(secName, "Keys", moduleKey.c_str(), ini);
    WritePrivateProfileStringA(secName, "Checked", checkedStr.c_str(), ini);
}

static void LoadSettings()
{
    std::string iniPath = GetIniPath();
    const char* ini = iniPath.c_str();
    const char* section = "Settings";

    char path[MAX_PATH] = "";
    GetPrivateProfileStringA(section, "OutputFolder", "", path, MAX_PATH, ini);
    if (hEditOutput)
        SetWindowTextA(hEditOutput, path);

    int skipSys = GetPrivateProfileIntA(section, "SkipSystemModules", 0, ini);
    int logCalls = GetPrivateProfileIntA(section, "LogSkippedCalls", 0, ini);
    int dump = GetPrivateProfileIntA(section, "DumpTracedModules", 0, ini);
    int finishAtFirstStop = GetPrivateProfileIntA(section, "FinishAtFirstStop", 1, ini);
    int skipModeIdx = GetPrivateProfileIntA(section, "SkipMode", 1, ini);
    if (hChkSkipSys)
        SendMessage(hChkSkipSys, BM_SETCHECK, skipSys ? BST_CHECKED : BST_UNCHECKED, 0);
    if (hChkLogCalls)
        SendMessage(hChkLogCalls, BM_SETCHECK, logCalls ? BST_CHECKED : BST_UNCHECKED, 0);
    if (hChkDump)
        SendMessage(hChkDump, BM_SETCHECK, dump ? BST_CHECKED : BST_UNCHECKED, 0);
    if (hChkFinishAtStop)
        SendMessage(hChkFinishAtStop, BM_SETCHECK, finishAtFirstStop ? BST_CHECKED : BST_UNCHECKED, 0);
    if (hCmbSkipMode)
        SendMessage(hCmbSkipMode, CB_SETCURSEL, (WPARAM)skipModeIdx, 0);
}

static void RestoreModuleChecksFromProfile()
{
    if (!hListModules || ListView_GetItemCount(hListModules) == 0)
        return;

    std::string iniPath = GetIniPath();
    const char* ini = iniPath.c_str();

    std::string moduleKey = BuildModuleSetKey();
    if (moduleKey.empty())
        return;

    int profileCount = GetPrivateProfileIntA("ModuleProfiles", "Count", 0, ini);

    for (int i = 0; i < profileCount; i++)
    {
        char secName[64];
        snprintf(secName, sizeof(secName), "ModuleSet_%d", i);

        char keyBuf[8192] = "";
        GetPrivateProfileStringA(secName, "Keys", "", keyBuf, sizeof(keyBuf), ini);

        if (moduleKey != keyBuf)
            continue;

        char checkedBuf[8192] = "";
        GetPrivateProfileStringA(secName, "Checked", "", checkedBuf, sizeof(checkedBuf), ini);

        std::set<std::string> checkedSet;
        if (checkedBuf[0] != '\0')
        {
            char* ctx = nullptr;
            char* tok = strtok_s(checkedBuf, ";", &ctx);
            while (tok)
            {
                checkedSet.insert(tok);
                tok = strtok_s(nullptr, ";", &ctx);
            }
        }

        int count = ListView_GetItemCount(hListModules);
        for (int j = 0; j < count; j++)
        {
            char text[MAX_MODULE_SIZE] = "";
            LVITEMA lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = j;
            lvi.pszText = text;
            lvi.cchTextMax = sizeof(text);
            SendMessageA(hListModules, LVM_GETITEMA, 0, (LPARAM)&lvi);

            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });

            ListView_SetCheckState(hListModules, j, checkedSet.count(lower) ? TRUE : FALSE);
        }

        Tracer::Instance().skipUserModules = checkedSet;
        return;
    }
}

static void RefreshModuleList()
{
    if (!hListModules)
        return;

    g_populatingList = true;

    std::set<std::string> wasChecked;
    int count = ListView_GetItemCount(hListModules);
    for (int i = 0; i < count; i++)
    {
        char text[MAX_MODULE_SIZE] = "";
        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = text;
        lvi.cchTextMax = sizeof(text);
        SendMessageA(hListModules, LVM_GETITEMA, 0, (LPARAM)&lvi);

        if (ListView_GetCheckState(hListModules, i))
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            wasChecked.insert(lower);
        }
    }

    bool hadItems = (count > 0);

    ListView_DeleteAllItems(hListModules);

    if (!DbgIsDebugging())
        return;

    BridgeList<Script::Module::ModuleInfo> modList;
    if (!Script::Module::GetList(&modList))
        return;

    int idx = 0;
    for (int i = 0; i < modList.Count(); i++)
    {
        duint base = modList[i].base;
        if (DbgFunctions()->ModGetParty(base) == mod_system)
            continue;

        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.pszText = modList[i].name;
        SendMessageA(hListModules, LVM_INSERTITEMA, 0, (LPARAM)&lvi);

        std::string lower(modList[i].name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        if (wasChecked.count(lower))
            ListView_SetCheckState(hListModules, idx, TRUE);

        idx++;
    }

    if (!hadItems && idx > 0)
        RestoreModuleChecksFromProfile();

    g_populatingList = false;
}

static void CollectSettings()
{
    Tracer& tracer = Tracer::Instance();

    char path[MAX_PATH] = "";
    GetWindowTextA(hEditOutput, path, MAX_PATH);
    tracer.outputFolder = path;

    tracer.skipSystemModules = (SendMessage(hChkSkipSys, BM_GETCHECK, 0, 0) == BST_CHECKED);
    tracer.logSkippedCalls = hChkLogCalls && (SendMessage(hChkLogCalls, BM_GETCHECK, 0, 0) == BST_CHECKED);

    int idx = hCmbSkipMode ? (int)SendMessage(hCmbSkipMode, CB_GETCURSEL, 0, 0) : 0;
    tracer.skipMode = (idx == 1) ? Tracer::SKIP_SILENT : Tracer::SKIP_STEP_OVER;

    tracer.dumpTracedModules = (SendMessage(hChkDump, BM_GETCHECK, 0, 0) == BST_CHECKED);
    tracer.finishAtFirstStop = hChkFinishAtStop && (SendMessage(hChkFinishAtStop, BM_GETCHECK, 0, 0) == BST_CHECKED);

    tracer.skipUserModules.clear();
    int count = ListView_GetItemCount(hListModules);
    for (int i = 0; i < count; i++)
    {
        if (!ListView_GetCheckState(hListModules, i))
            continue;

        char text[MAX_MODULE_SIZE] = "";
        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = text;
        lvi.cchTextMax = sizeof(text);
        SendMessageA(hListModules, LVM_GETITEMA, 0, (LPARAM)&lvi);

        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        tracer.skipUserModules.insert(lower);
    }
}

static void BrowseOutputFolder()
{
    BROWSEINFOA bi = {};
    bi.hwndOwner = hDialog;
    bi.lpszTitle = "Select trace output folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl)
    {
        char path[MAX_PATH] = "";
        if (SHGetPathFromIDListA(pidl, path))
            SetWindowTextA(hEditOutput, path);
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int notification = HIWORD(wParam);

        switch (id)
        {
        case IDC_BTN_BROWSE:
            BrowseOutputFolder();
            CollectSettings();
            SaveSettings();
            break;
        case IDC_BTN_REFRESH:
            RefreshModuleList();
            break;
        case IDC_BTN_SELALL:
            for (int i = 0, n = ListView_GetItemCount(hListModules); i < n; i++)
                ListView_SetCheckState(hListModules, i, TRUE);
            CollectSettings();
            SaveSettings();
            break;
        case IDC_BTN_DESALL:
            for (int i = 0, n = ListView_GetItemCount(hListModules); i < n; i++)
                ListView_SetCheckState(hListModules, i, FALSE);
            CollectSettings();
            SaveSettings();
            break;
        case IDC_BTN_REVSEL:
            for (int i = 0, n = ListView_GetItemCount(hListModules); i < n; i++)
                ListView_SetCheckState(hListModules, i, !ListView_GetCheckState(hListModules, i));
            CollectSettings();
            SaveSettings();
            break;
        case IDC_CHK_SKIPSYS:
        case IDC_CHK_LOGCALLS:
        case IDC_CHK_DUMP:
        case IDC_CHK_FINISH_AT_STOP:
            CollectSettings();
            SaveSettings();
            break;
        case IDC_CMB_SKIPMODE:
            if (notification == CBN_SELCHANGE)
            {
                CollectSettings();
                SaveSettings();
            }
            break;
        case IDC_EDT_OUTPUT:
            if (notification == EN_KILLFOCUS)
            {
                CollectSettings();
                SaveSettings();
            }
            break;
        case IDC_BTN_START:
            RefreshModuleList();
            CollectSettings();
            SaveSettings();
            Tracer::Instance().Start();
            EnableWindow(hBtnStart, FALSE);
            EnableWindow(hBtnStop, TRUE);
            break;
        case IDC_BTN_STOP:
            Tracer::Instance().Stop();
            break;
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom == IDC_LV_MODULES && hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            if (!g_populatingList &&
                (nmlv->uChanged & LVIF_STATE) &&
                ((nmlv->uOldState ^ nmlv->uNewState) & LVIS_STATEIMAGEMASK))
            {
                CollectSettings();
                SaveSettings();
            }
        }
        return 0;
    }

    case WM_TIMER:
    {
        DialogUpdateStatus();

        bool isRunning = Tracer::Instance().running.load();
        bool isPaused = Tracer::Instance().paused.load();

        if (isRunning)
        {
            SetWindowTextA(hBtnStart, "Continue Tracing");
            EnableWindow(hBtnStart, FALSE);
            EnableWindow(hBtnStop, TRUE);
        }
        else if (isPaused)
        {
            SetWindowTextA(hBtnStart, "Continue Tracing");
            bool canResume = DbgIsDebugging() && !DbgIsRunning();
            EnableWindow(hBtnStart, canResume);
            EnableWindow(hBtnStop, TRUE);
        }
        else
        {
            SetWindowTextA(hBtnStart, "Start Trace");
            bool canStart = DbgIsDebugging() && !DbgIsRunning();
            EnableWindow(hBtnStart, canStart);
            EnableWindow(hBtnStop, FALSE);
        }
        return 0;
    }

    case WM_SIZE:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int pad = 12;
        int btnH = 30;
        int listTop = 252;

        int statusH = 92;
        int sepH = 2;
        int sepGap = 6;
        int statusY = h - pad - statusH;
        int sepY = statusY - sepGap - sepH;

        int listH = sepY - listTop - pad;
        if (listH < 60) listH = 60;

        if (hEditOutput)
            MoveWindow(hEditOutput, pad, 36, w - pad * 3 - 80, 24, TRUE);
        if (HWND hBrowse = GetDlgItem(hwnd, IDC_BTN_BROWSE))
            MoveWindow(hBrowse, w - pad - 80, 36, 80, 24, TRUE);
        if (hBtnRefresh)
            MoveWindow(hBtnRefresh, w - pad - 80, 224, 80, 22, TRUE);
        if (hListModules)
            MoveWindow(hListModules, pad, listTop, w - pad * 2, listH, TRUE);
        if (hSeparator)
            MoveWindow(hSeparator, pad, sepY, w - pad * 2, sepH, TRUE);

        int lblW = w - pad * 3 - 120 - 8;
        if (hLblTime)
            MoveWindow(hLblTime, pad, statusY, lblW, 20, TRUE);
        if (hLblInstructions)
            MoveWindow(hLblInstructions, pad, statusY + 24, lblW, 20, TRUE);
        if (hLblStatus)
            MoveWindow(hLblStatus, pad, statusY + 48, lblW, 20, TRUE);
        if (hChkFinishAtStop)
            MoveWindow(hChkFinishAtStop, pad, statusY + 72, lblW, 20, TRUE);

        int btnX = w - pad - 120;
        if (hBtnStart)
            MoveWindow(hBtnStart, btnX, statusY, 120, btnH, TRUE);
        if (hBtnStop)
            MoveWindow(hBtnStop, btnX, statusY + 36, 120, btnH, TRUE);

        return 0;
    }

    case WM_CLOSE:
        SaveSettings();
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (timerID)
        {
            KillTimer(hwnd, timerID);
            timerID = 0;
        }
        hDialog = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DialogShow(HWND parent)
{
    if (hDialog)
    {
        ShowWindow(hDialog, SW_SHOW);
        SetForegroundWindow(hDialog);
        RefreshModuleList();
        return;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DIALOG_CLASS;
    RegisterClassExW(&wc);

    hDialog = CreateWindowExW(
        0, DIALOG_CLASS, L"Extended Trace",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 540, 640,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);

    int pad = 12;
    int y = pad;

    CreateWindowA("STATIC", "Artifacts folder:", WS_CHILD | WS_VISIBLE,
        pad, y, 120, 20, hDialog, nullptr, nullptr, nullptr);
    y += 24;

    hEditOutput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad, y, 340, 24, hDialog, (HMENU)IDC_EDT_OUTPUT, nullptr, nullptr);

    CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        360, y, 80, 24, hDialog, (HMENU)IDC_BTN_BROWSE, nullptr, nullptr);
    y += 38;

    CreateWindowA("STATIC", "Skip behavior:",
        WS_CHILD | WS_VISIBLE,
        pad, y + 3, 100, 20, hDialog, nullptr, nullptr, nullptr);

    hCmbSkipMode = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        pad + 104, y, 380, 200, hDialog, (HMENU)IDC_CMB_SKIPMODE, nullptr, nullptr);
    SendMessageA(hCmbSkipMode, CB_ADDSTRING, 0, (LPARAM)"Step Over - stop trace on call, resume on return");
    SendMessageA(hCmbSkipMode, CB_ADDSTRING, 0, (LPARAM)"Silent - trace w/o processing instructions");
    SendMessage(hCmbSkipMode, CB_SETCURSEL, 1, 0);
    y += 30;

    hChkSkipSys = CreateWindowA("BUTTON", "Skip system modules",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        pad, y, 480, 20, hDialog, (HMENU)IDC_CHK_SKIPSYS, nullptr, nullptr);
    SendMessage(hChkSkipSys, BM_SETCHECK, BST_UNCHECKED, 0);
    y += 28;

    hChkLogCalls = CreateWindowA("BUTTON", "Log calls in skipped modules (silent mode only)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        pad, y, 480, 20, hDialog, (HMENU)IDC_CHK_LOGCALLS, nullptr, nullptr);
    y += 28;

    hChkDump = CreateWindowA("BUTTON", "Dump traced user modules on finish",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        pad, y, 480, 20, hDialog, (HMENU)IDC_CHK_DUMP, nullptr, nullptr);
    SendMessage(hChkDump, BM_SETCHECK, BST_UNCHECKED, 0);
    y += 36;

    CreateWindowA("STATIC", "User modules to skip:",
        WS_CHILD | WS_VISIBLE,
        pad, y, 320, 20, hDialog, nullptr, nullptr, nullptr);
    y += 28;

    CreateWindowA("BUTTON", "Select All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        pad, y, 90, 22, hDialog, (HMENU)IDC_BTN_SELALL, nullptr, nullptr);
    CreateWindowA("BUTTON", "Deselect All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        pad + 96, y, 90, 22, hDialog, (HMENU)IDC_BTN_DESALL, nullptr, nullptr);
    CreateWindowA("BUTTON", "Reverse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        pad + 192, y, 80, 22, hDialog, (HMENU)IDC_BTN_REVSEL, nullptr, nullptr);
    hBtnRefresh = CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        440, y, 80, 22, hDialog, (HMENU)IDC_BTN_REFRESH, nullptr, nullptr);
    y += 28;

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    hListModules = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
        pad, y, 490, 250, hDialog, (HMENU)IDC_LV_MODULES, nullptr, nullptr);

    ListView_SetExtendedListViewStyle(hListModules,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNA col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (char*)"Module Name";
    col.cx = 460;
    SendMessageA(hListModules, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    y += 260;

    hSeparator = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        pad, y, 490, 2, hDialog, nullptr, nullptr, nullptr);
    y += 10;

    hLblTime = CreateWindowA("STATIC", "Time: --",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y, 300, 20, hDialog, nullptr, nullptr, nullptr);
    hLblInstructions = CreateWindowA("STATIC", "Instructions: 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y + 24, 300, 20, hDialog, nullptr, nullptr, nullptr);
    hLblStatus = CreateWindowA("STATIC", "Status: Idle",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y + 48, 300, 20, hDialog, (HMENU)IDC_LBL_STATUS, nullptr, nullptr);

    hChkFinishAtStop = CreateWindowA("BUTTON", "Finish trace at first stop",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        pad, y + 72, 360, 20, hDialog, (HMENU)IDC_CHK_FINISH_AT_STOP, nullptr, nullptr);
    SendMessage(hChkFinishAtStop, BM_SETCHECK, BST_CHECKED, 0);

    hBtnStart = CreateWindowA("BUTTON", "Start Trace",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        392, y, 120, 30, hDialog, (HMENU)IDC_BTN_START, nullptr, nullptr);
    hBtnStop = CreateWindowA("BUTTON", "Finish Trace",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        392, y + 36, 120, 30, hDialog, (HMENU)IDC_BTN_STOP, nullptr, nullptr);
    EnableWindow(hBtnStop, FALSE);

    bool canStart = DbgIsDebugging() && !DbgIsRunning();
    EnableWindow(hBtnStart, canStart);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(hDialog, [](HWND child, LPARAM lp) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)hFont);

    LoadSettings();

    timerID = SetTimer(hDialog, 1, 200, nullptr);

    ShowWindow(hDialog, SW_SHOW);
    UpdateWindow(hDialog);

    if (DbgIsDebugging())
        RefreshModuleList();
}

void DialogDestroy()
{
    if (hDialog)
    {
        DestroyWindow(hDialog);
        hDialog = nullptr;
    }
}

void DialogUpdateStatus()
{
    Tracer& tracer = Tracer::Instance();
    bool tracing = tracer.running.load();
    bool sessionPaused = tracer.paused.load();
    bool sessionFinished = tracer.finished.load();

    if (hLblTime)
    {
        char buf[64];
        ULONGLONG startTick = tracer.traceStartTick.load();
        ULONGLONG endTick = tracer.traceEndTick.load();
        if (startTick != 0)
        {
            ULONGLONG elapsed = (tracing || sessionPaused)
                ? (GetTickCount64() - startTick)
                : (endTick ? endTick - startTick : 0);
            DWORD secs = (DWORD)(elapsed / 1000);
            snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d",
                secs / 3600, (secs % 3600) / 60, secs % 60);
        }
        else
            snprintf(buf, sizeof(buf), "Time: --");

        SetWindowTextA(hLblTime, buf);
    }

    if (hLblInstructions)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Instructions: %llu",
            (unsigned long long)tracer.traceCounter.load());
        SetWindowTextA(hLblInstructions, buf);
    }

    if (hLblStatus)
    {
        const char* status;
        if (tracing)
            status = "Status: Tracing";
        else if (sessionPaused)
            status = "Status: Session paused";
        else if (sessionFinished)
            status = "Status: Finished";
        else if (DbgIsDebugging() && DbgIsRunning())
            status = "Status: Running";
        else if (DbgIsDebugging())
            status = "Status: Paused";
        else
            status = "Status: Idle";
        SetWindowTextA(hLblStatus, status);
    }
}

void DialogRefreshModules()
{
    if (hDialog)
        RefreshModuleList();
}

void DialogOnDebugStop()
{
    if (hListModules)
        ListView_DeleteAllItems(hListModules);
}
