#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winternl.h>

// No standard-library containers, CRT startup, sampling, worker thread or watchdog.
constexpr wchar_t WindowClass[] = L"VramSave.DesktopToggle";
constexpr UINT StopMessage = WM_APP + 1, TrayMessage = WM_APP + 2;
constexpr UINT ToggleCommand = 1001, ExitCommand = 1002;
const wchar_t* Targets[] = {L"dwm.exe", L"explorer.exe",
    L"StartMenuExperienceHost.exe", L"ShellExperienceHost.exe", L"winlogon.exe"};
using NtProcessFn = LONG (NTAPI*)(HANDLE);
using QueryFn = LONG (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
enum class Mode { Desktop, Off, Restoring, RestoreFailed };
Mode mode = Mode::Desktop;
HWND mainWindow;
DWORD sessionId;
UINT taskbarCreated;
NOTIFYICONDATAW tray;
HANDLE winlogonHandle, shellToken, shellHelper;
bool quitAfterRestore;
NtProcessFn suspendProcess, resumeProcess;
QueryFn queryProcesses;
wchar_t* executable;
wchar_t* shellIds[2];
UINT hotkeyModifiers = MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT;
UINT hotkeyKey = 'M';
wchar_t hotkeyText[48] = L"Ctrl+Shift+Alt+M";

// Compiler-generated aggregate initialization can call memset even without CRT.
extern "C" void* __cdecl memset(void*, int, size_t);
#pragma function(memset)
extern "C" void* __cdecl memset(void* destination, int value, size_t count) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(destination);
    while (count--) *p++ = static_cast<unsigned char>(value);
    return destination;
}

void* Allocate(SIZE_T bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes); }
void Release(void* memory) { if (memory) HeapFree(GetProcessHeap(), 0, memory); }

void Append(wchar_t* output, SIZE_T capacity, const wchar_t* text) {
    SIZE_T used = static_cast<SIZE_T>(lstrlenW(output));
    while (*text && used + 1 < capacity) output[used++] = *text++;
    output[used] = 0;
}
template<SIZE_T N> void Append(wchar_t (&output)[N], const wchar_t* text) {
    Append(output, N, text);
}

wchar_t* CopyText(const wchar_t* text) {
    SIZE_T count = static_cast<SIZE_T>(lstrlenW(text)) + 1;
    auto copy = static_cast<wchar_t*>(Allocate(count * sizeof(wchar_t)));
    if (copy) { copy[0] = 0; Append(copy, count, text); }
    return copy;
}

void ShowError(const wchar_t* operation, DWORD code) {
    wchar_t message[384] = L"";
    wchar_t number[11] = L"0x00000000";
    for (unsigned i = 0; i < 8; ++i)
        number[9 - i] = L"0123456789ABCDEF"[(code >> (i * 4)) & 15];
    Append(message, operation); Append(message, L"\n错误代码："); Append(message, number);
    MessageBoxW(mainWindow, message, L"DesktopToggle", MB_OK | MB_ICONERROR);
}

bool ParseHotkey(const wchar_t* value) {
    UINT modifiers = MOD_NOREPEAT, key = 0;
    for (;;) {
        wchar_t token[8] = {};
        unsigned length = 0;
        while (*value && *value != L'+') {
            wchar_t c = *value++;
            if (c == L' ') continue;
            if (length == ARRAYSIZE(token) - 1) return false;
            token[length++] = c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c;
        }
        if (lstrcmpW(token, L"CTRL") == 0) modifiers |= MOD_CONTROL;
        else if (lstrcmpW(token, L"SHIFT") == 0) modifiers |= MOD_SHIFT;
        else if (lstrcmpW(token, L"ALT") == 0) modifiers |= MOD_ALT;
        else if (length == 1 && !key &&
            ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9'))) key = token[0];
        else return false;
        if (!*value) break;
        ++value;
    }
    if (!key || modifiers == MOD_NOREPEAT) return false;
    hotkeyModifiers = modifiers; hotkeyKey = key; hotkeyText[0] = 0;
    if (modifiers & MOD_CONTROL) Append(hotkeyText, L"Ctrl+");
    if (modifiers & MOD_SHIFT) Append(hotkeyText, L"Shift+");
    if (modifiers & MOD_ALT) Append(hotkeyText, L"Alt+");
    wchar_t letter[] = {static_cast<wchar_t>(key), 0};
    Append(hotkeyText, letter);
    return true;
}

const wchar_t* ActionText() {
    if (mode == Mode::Desktop) return L"关闭桌面";
    if (mode == Mode::Restoring) return L"正在恢复桌面…";
    if (mode == Mode::RestoreFailed) return L"重试恢复桌面";
    return L"恢复桌面";
}

void AddTray() {
    tray.szTip[0] = 0;
    Append(tray.szTip, L"DesktopToggle 0.8 - "); Append(tray.szTip, hotkeyText);
    if (Shell_NotifyIconW(NIM_ADD, &tray)) {
        tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray);
    }
}

// A snapshot exists only during a toggle or a short DWM startup check.
// The session ID is read directly: protected processes need not be opened.
SYSTEM_PROCESS_INFORMATION* Snapshot() {
    ULONG bytes = 0;
    queryProcesses(SystemProcessInformation, nullptr, 0, &bytes);
    ULONG capacity = bytes + 65536;
    auto data = static_cast<SYSTEM_PROCESS_INFORMATION*>(
        VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data) return nullptr;
    if (queryProcesses(SystemProcessInformation, data, capacity, &bytes) < 0) {
        VirtualFree(data, 0, MEM_RELEASE); return nullptr;
    }
    return data;
}

void ReleaseSnapshot(SYSTEM_PROCESS_INFORMATION* list) {
    if (list) VirtualFree(list, 0, MEM_RELEASE);
}

SYSTEM_PROCESS_INFORMATION* Next(SYSTEM_PROCESS_INFORMATION* entry) {
    return entry->NextEntryOffset ? reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(
        reinterpret_cast<unsigned char*>(entry) + entry->NextEntryOffset) : nullptr;
}

bool IsTarget(SYSTEM_PROCESS_INFORMATION* entry, unsigned target) {
    return entry->SessionId == sessionId && entry->ImageName.Buffer &&
        CompareStringOrdinal(entry->ImageName.Buffer, entry->ImageName.Length / sizeof(wchar_t),
            Targets[target], -1, TRUE) == CSTR_EQUAL;
}

DWORD FindProcess(SYSTEM_PROCESS_INFORMATION* list, unsigned target) {
    for (auto entry = list; entry; entry = Next(entry))
        if (IsTarget(entry, target)) return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->UniqueProcessId));
    return 0;
}

void ReadShellIds(SYSTEM_PROCESS_INFORMATION* list) {
    const wchar_t* defaults[] = {
        L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!FullTrustApp",
        L"Microsoft.Windows.ShellExperienceHost_cw5n1h2txyewy!App"
    };
    for (unsigned i = 0; i < 2; ++i) {
        Release(shellIds[i]); shellIds[i] = CopyText(defaults[i]);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, FindProcess(list, i + 2));
        if (!process) continue;
        UINT32 count = 0;
        if (GetApplicationUserModelId(process, &count, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
            auto id = static_cast<wchar_t*>(Allocate(count * sizeof(wchar_t)));
            if (id && GetApplicationUserModelId(process, &count, id) == ERROR_SUCCESS) {
                Release(shellIds[i]); shellIds[i] = id;
            } else Release(id);
        }
        CloseHandle(process);
    }
}

bool EnableDebugPrivilege() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid);
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    DWORD error = GetLastError();
    CloseHandle(token);
    return ok && error == ERROR_SUCCESS;
}

bool CaptureShellToken() {
    DWORD pid = 0; GetWindowThreadProcessId(GetShellWindow(), &pid);
    DWORD sid = 0;
    if (!pid || !ProcessIdToSessionId(pid, &sid) || sid != sessionId) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    BOOL ok = OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &token);
    if (ok) ok = DuplicateTokenEx(token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &shellToken);
    if (token) CloseHandle(token);
    CloseHandle(process);
    return ok != FALSE;
}

struct TargetHandle { HANDLE process; unsigned target; };

// Open all targets before terminating any: the later stages use handles, not
// stale PIDs for processes that Explorer's shutdown may already have removed.
TargetHandle* OpenTargets(SYSTEM_PROCESS_INFORMATION* list, unsigned& count) {
    count = 0;
    for (auto entry = list; entry; entry = Next(entry))
        for (unsigned i = 0; i < 4; ++i) if (IsTarget(entry, i)) ++count;
    auto handles = static_cast<TargetHandle*>(Allocate((count ? count : 1) * sizeof(TargetHandle)));
    if (!handles) return nullptr;
    count = 0;
    for (auto entry = list; entry; entry = Next(entry)) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!IsTarget(entry, i)) continue;
            DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->UniqueProcessId));
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (process) handles[count++] = {process, i};
        }
    }
    return handles;
}

void CloseTargets(TargetHandle* handles, unsigned count) {
    for (unsigned i = 0; i < count; ++i) CloseHandle(handles[i].process);
    Release(handles);
}

void KillTargets(TargetHandle* handles, unsigned count) {
    const unsigned order[] = {1, 2, 3, 0};
    for (unsigned target : order) {
        for (unsigned i = 0; i < count; ++i) {
            if (handles[i].target != target) continue;
            HANDLE process = handles[i].process;
            if (TerminateProcess(process, 0)) WaitForSingleObject(process, 1000);
        }
    }
}

void DesktopOff() {
    auto list = Snapshot();
    if (!list) { ShowError(L"无法获取进程列表。", ERROR_GEN_FAILURE); return; }
    ReadShellIds(list);
    if (!shellIds[0] || !shellIds[1] || !CaptureShellToken()) {
        ReleaseSnapshot(list); ShowError(L"无法准备桌面恢复信息。", ERROR_GEN_FAILURE); return;
    }
    winlogonHandle = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, FindProcess(list, 4));
    unsigned count = 0;
    auto targets = OpenTargets(list, count);
    ReleaseSnapshot(list);
    if (!targets) {
        if (winlogonHandle) CloseHandle(winlogonHandle);
        winlogonHandle = nullptr;
        CloseHandle(shellToken); shellToken = nullptr;
        ShowError(L"无法准备目标进程句柄。", ERROR_NOT_ENOUGH_MEMORY); return;
    }
    LONG status = winlogonHandle ? suspendProcess(winlogonHandle) : static_cast<LONG>(0xC0000022);
    if (status < 0) {
        if (winlogonHandle) CloseHandle(winlogonHandle);
        winlogonHandle = nullptr;
        CloseHandle(shellToken); shellToken = nullptr; CloseTargets(targets, count);
        ShowError(L"无法挂起 Winlogon。", static_cast<DWORD>(status)); return;
    }
    mode = Mode::Off;
    KillTargets(targets, count);
    CloseTargets(targets, count);
}

void RestoreFailed(DWORD error) {
    KillTimer(mainWindow, 1);
    mode = Mode::RestoreFailed;

    ShowError(L"桌面恢复未完成。请再次按快捷键或选择“重试恢复桌面”。", error);
}

void DesktopRestore() {
    if (winlogonHandle) {
        LONG status = resumeProcess(winlogonHandle);
        if (status < 0) { RestoreFailed(static_cast<DWORD>(status)); return; }
        CloseHandle(winlogonHandle); winlogonHandle = nullptr;
    }
    mode = Mode::Restoring;
    // Only wait for DWM with a timer. Helper completion uses its process handle.
    if (!SetTimer(mainWindow, 1, 250, nullptr)) RestoreFailed(GetLastError());
}

void AdvanceRestore() {
    if (mode != Mode::Restoring || shellHelper) return;
    auto list = Snapshot();
    bool dwmReady = FindProcess(list, 0) != 0;
    ReleaseSnapshot(list);
    if (!dwmReady) return;
    KillTimer(mainWindow, 1);
    SIZE_T capacity = static_cast<SIZE_T>(lstrlenW(executable)) + lstrlenW(shellIds[0]) + lstrlenW(shellIds[1]) + 32;
    auto command = static_cast<wchar_t*>(Allocate(capacity * sizeof(wchar_t)));
    if (!command) { RestoreFailed(ERROR_NOT_ENOUGH_MEMORY); return; }
    command[0] = 0;
    Append(command, capacity, L"\""); Append(command, capacity, executable);
    Append(command, capacity, L"\" --restore-shell \""); Append(command, capacity, shellIds[0]);
    Append(command, capacity, L"\" \""); Append(command, capacity, shellIds[1]); Append(command, capacity, L"\"");
    STARTUPINFOW startup{sizeof(startup)};
    wchar_t desktop[] = L"winsta0\\default"; startup.lpDesktop = desktop;
    PROCESS_INFORMATION info{};
    BOOL ok = CreateProcessWithTokenW(shellToken, 0, executable, command,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    DWORD error = ok ? 0 : GetLastError();
    Release(command);
    if (!ok) { RestoreFailed(error); return; }
    shellHelper = info.hProcess; CloseHandle(info.hThread);
}

void FinishRestore() {
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(shellHelper, &code);
    DWORD error = ok ? code : GetLastError();
    CloseHandle(shellHelper); shellHelper = nullptr;
    if (error) { RestoreFailed(error); return; }
    CloseHandle(shellToken); shellToken = nullptr;
    for (auto& id : shellIds) { Release(id); id = nullptr; }
    mode = Mode::Desktop;
    if (quitAfterRestore) PostQuitMessage(0);
}

// Helper runs with the original Explorer token; COM stays on this short-lived process.
DWORD RestoreShell(const wchar_t* startId, const wchar_t* shellId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return static_cast<DWORD>(hr);
    if (!GetShellWindow()) {
        wchar_t windows[MAX_PATH], path[MAX_PATH + 16] = L"";
        GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
        Append(path, windows); Append(path, L"\\explorer.exe");
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION info{};
        if (!CreateProcessW(path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, windows, &startup, &info)) {
            DWORD error = GetLastError(); CoUninitialize(); return error;
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess);
    }
    IApplicationActivationManager* manager = nullptr;
    hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) { CoUninitialize(); return static_cast<DWORD>(hr); }
    CoAllowSetForegroundWindow(manager, nullptr);
    const wchar_t* ids[] = {startId, shellId};
    HRESULT result = S_OK;
    for (const auto* id : ids) {
        DWORD pid = 0;
        hr = manager->ActivateApplication(id, L"", AO_NOERRORUI, &pid);
        if (FAILED(hr)) result = hr;
    }
    manager->Release(); CoUninitialize();
    return static_cast<DWORD>(result);
}

void ToggleDesktop() {
    if (mode == Mode::Desktop) DesktopOff();
    else if (mode != Mode::Restoring) DesktopRestore();
}

void RequestExit() {
    quitAfterRestore = true;
    if (mode == Mode::Desktop) PostQuitMessage(0);
    else if (mode != Mode::Restoring) DesktopRestore();
}

void ShowTrayMenu() {
    POINT point{};
    NOTIFYICONIDENTIFIER icon{sizeof(icon)};
    icon.hWnd = mainWindow; icon.uID = tray.uID;
    TPMPARAMS placement{sizeof(placement)};
    bool hasIconRect = SUCCEEDED(Shell_NotifyIconGetRect(&icon, &placement.rcExclude));
    if (hasIconRect) point = {placement.rcExclude.left, placement.rcExclude.bottom};
    else GetCursorPos(&point);
    SetWindowPos(mainWindow, nullptr, point.x, point.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    HMENU menu = CreatePopupMenu();
    wchar_t label[96] = L"";
    Append(label, ActionText()); Append(label, L"\t"); Append(label, hotkeyText);
    AppendMenuW(menu, MF_STRING | (mode == Mode::Restoring ? MF_GRAYED : 0), ToggleCommand, label);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ExitCommand, L"退出程序");
    SetForegroundWindow(mainWindow);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, mainWindow, hasIconRect ? &placement : nullptr);
    DestroyMenu(menu);
    PostMessageW(mainWindow, WM_NULL, 0, 0);
    if (command) PostMessageW(mainWindow, WM_COMMAND, command, 0);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated && message == taskbarCreated) { AddTray(); return 0; }
    switch (message) {
    case TrayMessage:
        if (HIWORD(lParam) == tray.uID && LOWORD(lParam) == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ToggleCommand) ToggleDesktop();
        else if (LOWORD(wParam) == ExitCommand) RequestExit();
        return 0;
    case WM_HOTKEY:
        if (wParam == 1) ToggleDesktop();
        return 0;
    case WM_TIMER:
        if (wParam == 1) AdvanceRestore();
        return 0;
    case StopMessage:
    case WM_CLOSE:
        RequestExit(); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool Elevated() {
    HANDLE token; TOKEN_ELEVATION elevation{}; DWORD size;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

DWORD MessageLoop() {
    for (;;) {
        DWORD count = shellHelper ? 1 : 0;
        DWORD result = MsgWaitForMultipleObjectsEx(count, count ? &shellHelper : nullptr,
            INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) return GetLastError();
        if (count && result == WAIT_OBJECT_0) FinishRestore();
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<DWORD>(message.wParam);
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}

__declspec(noinline) DWORD Run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    bool stop = false, checkHotkey = false;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpW(argv[i], L"--restore-shell") == 0 && i + 2 < argc) {
            DWORD result = RestoreShell(argv[i + 1], argv[i + 2]); LocalFree(argv); return result;
        }
        if (lstrcmpW(argv[i], L"--stop") == 0) stop = true;
        else if (lstrcmpW(argv[i], L"--check-hotkey") == 0) checkHotkey = true;
        else if (lstrcmpW(argv[i], L"--hotkey") == 0 && i + 1 < argc && ParseHotkey(argv[++i])) {}
        else {
            LocalFree(argv);
            ShowError(L"支持参数：--stop、--check-hotkey、--hotkey Ctrl+Shift+Alt+M。", ERROR_INVALID_PARAMETER);
            return 6;
        }
    }
    LocalFree(argv);
    if (checkHotkey) {
        if (!RegisterHotKey(nullptr, 1, hotkeyModifiers, hotkeyKey)) return 5;
        UnregisterHotKey(nullptr, 1); return 0;
    }
    auto path = static_cast<wchar_t*>(Allocate(32768 * sizeof(wchar_t)));
    if (!path) return 1;
    DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    executable = length && length < 32768 ? CopyText(path) : nullptr;
    Release(path);
    if (!executable) return 1;
    if (!Elevated()) {
        wchar_t arguments[80] = L"";
        if (stop) Append(arguments, L"--stop");
        else { Append(arguments, L"--hotkey \""); Append(arguments, hotkeyText); Append(arguments, L"\""); }
        auto result = ShellExecuteW(nullptr, L"runas", executable, arguments, nullptr, SW_HIDE);
        Release(executable);
        return reinterpret_cast<INT_PTR>(result) > 32 ? 0 : 1;
    }
    if (stop) {
        HWND target = FindWindowW(WindowClass, nullptr);
        Release(executable);
        return target && PostMessageW(target, StopMessage, 0, 0) ? 0 : 1;
    }
    if (FindWindowW(WindowClass, nullptr)) { Release(executable); return 0; }
    if (!EnableDebugPrivilege()) { ShowError(L"无法启用调试权限。", ERROR_PRIVILEGE_NOT_HELD); Release(executable); return 3; }
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    suspendProcess = reinterpret_cast<NtProcessFn>(GetProcAddress(ntdll, "NtSuspendProcess"));
    resumeProcess = reinterpret_cast<NtProcessFn>(GetProcAddress(ntdll, "NtResumeProcess"));
    queryProcesses = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!suspendProcess || !resumeProcess || !queryProcesses) { Release(executable); return 3; }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc; wc.lpszClassName = WindowClass;
    RegisterClassW(&wc);
    mainWindow = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, L"DesktopToggle", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!mainWindow) { Release(executable); return 4; }
    if (!RegisterHotKey(mainWindow, 1, hotkeyModifiers, hotkeyKey)) {
        ShowError(L"无法注册快捷键。请关闭占用程序或使用 --hotkey 指定其他组合。", GetLastError());
        DestroyWindow(mainWindow); Release(executable); return 5;
    }
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(mainWindow, taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(mainWindow, TrayMessage, MSGFLT_ALLOW, nullptr);
    tray.cbSize = sizeof(tray); tray.hWnd = mainWindow; tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    tray.uCallbackMessage = TrayMessage; tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    AddTray();
    DWORD result = MessageLoop();
    KillTimer(mainWindow, 1);
    Shell_NotifyIconW(NIM_DELETE, &tray);
    UnregisterHotKey(mainWindow, 1); DestroyWindow(mainWindow);
    for (auto id : shellIds) Release(id);
    Release(executable);
    return result;
}

// Keep /GS enabled: initialize its cookie before entering any protected function.
extern "C" void __cdecl __security_init_cookie();
extern "C" void WINAPI EntryPoint() {
    __security_init_cookie();
    ExitProcess(Run());
}
