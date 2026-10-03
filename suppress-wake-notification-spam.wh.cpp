// ==WindhawkMod==
// @id              suppress-wake-notification-spam
// @name            Suppress Wake Notification Spam
// @description     Prevents queued notification banners and sounds from flooding the desktop after wake or unlock while keeping notifications in Notification Center.
// @version         1.0.0
// @author          YiftahCooper
// @github          https://github.com/YiftahCooper
// @homepage        https://github.com/YiftahCooper/Suppress-Wake-Notification-Spam
// @license         MIT
// @include         windhawk.exe
// @compilerOptions -lole32 -lwtsapi32 -luuid -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Suppress Wake Notification Spam

This mod temporarily enables Windows' own Do Not Disturb profile while the
interactive session is away:

- the display is powered off;
- the Windows session is locked; or
- the system is suspending/asleep.

When the display/session becomes active again, the mod waits briefly and then
restores the exact user-selected DND profile that existed before the mod made
its temporary change.

The important distinction is that the mod does **not** delete notifications.
Windows still receives them and keeps them in Notification Center; the mod is
intended only to suppress banners and notification sounds while you are away,
so Windows has no backlog of banners to replay when you return.

## Safety behavior

- If DND was already enabled before the display went off / the PC was locked,
  the mod leaves it alone and does not turn it off later.
- If the DND profile changes while the mod owns the temporary change, the mod
  refuses to overwrite the newer profile on restore.
- A small persistent ownership marker is used so that, if Windhawk is killed
  while the mod temporarily owns DND, the next start can restore the previous
  profile rather than leaving DND stuck on.

## Privacy and logging

The mod does not inspect notification payloads, notification text, senders,
subjects, or message contents. It does not need to hook individual apps or
notifications at all. Diagnostic logs contain only display/session power state,
DND profile state, timing, and API/error information.

## Implementation

The mod uses the same undocumented QuietHoursSettings COM service used by
Windows-aware desktop applications, rather than editing the CloudStore binary
registry data. Display state comes from GUID_SESSION_DISPLAY_STATUS and
lock/unlock state from WTS session notifications.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- restoreDelayMs: 2500
  $name: Restore delay after wake/unlock (ms)
  $description: How long to keep DND enabled after the display/session becomes active. 2500 ms is a conservative starting value for absorbing wake-time notification replay.
- suppressionProfile: priority
  $name: Temporary suppression profile
  $description: Priority is the normal Windows 11 Do Not Disturb profile. Alarms only is stricter and can also suppress reminder-style notifications that may bypass ordinary DND.
  $options:
  - priority: Priority only (normal Windows DND)
  - alarms: Alarms only (stricter)
- suppressOnDisplayOff: true
  $name: Suppress while display is off
- suppressOnLock: true
  $name: Suppress while Windows is locked
- suppressOnSuspend: true
  $name: Suppress while system is suspended
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include <windhawk_utils.h>

namespace {

constexpr wchar_t kProfileUnrestricted[] =
    L"Microsoft.QuietHoursProfile.Unrestricted";
constexpr wchar_t kProfilePriorityOnly[] =
    L"Microsoft.QuietHoursProfile.PriorityOnly";
constexpr wchar_t kProfileAlarmsOnly[] =
    L"Microsoft.QuietHoursProfile.AlarmsOnly";

// QuietHoursSettings COM service. The interface layout is intentionally kept
// minimal: IUnknown + the first two IQuietHoursSettings methods.
const CLSID kClsidQuietHoursSettings = {
    0xF53321FA,
    0x34F8,
    0x4B7F,
    {0xB9, 0xA3, 0x36, 0x18, 0x77, 0xCB, 0x94, 0xCF}};

const IID kIidQuietHoursSettings = {
    0x6BFF4732,
    0x81EC,
    0x4FFB,
    {0xAE, 0x67, 0xB6, 0xC1, 0xBC, 0x29, 0x63, 0x1F}};

// GUID_SESSION_DISPLAY_STATUS. Defined locally so the mod doesn't depend on
// the exact age of the Windows SDK headers bundled with Windhawk.
const GUID kGuidSessionDisplayStatus = {
    0x2B84C20E,
    0xAD23,
    0x4DDF,
    {0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}};

struct IQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(
        LPWSTR* profileId) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(
        LPWSTR profileId) = 0;
};

enum class SuppressionProfile : int {
    Priority = 0,
    Alarms = 1,
};

struct Settings {
    std::atomic<DWORD> restoreDelayMs{2500};
    std::atomic<SuppressionProfile> suppressionProfile{
        SuppressionProfile::Priority};
    std::atomic<bool> suppressOnDisplayOff{true};
    std::atomic<bool> suppressOnLock{true};
    std::atomic<bool> suppressOnSuspend{true};
} g_settings;

HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_readyEvent = nullptr;

// These state variables are only touched by the worker/message-loop thread.
bool g_displayOff = false;
bool g_sessionLocked = false;
bool g_suspended = false;
bool g_ownsTemporaryDnd = false;
std::wstring g_originalProfile;
std::wstring g_targetProfile;

constexpr UINT_PTR kRestoreTimerId = 1;
constexpr UINT kMsgSettingsChanged = WM_APP + 1;

constexpr wchar_t kPersistOwnsDnd[] = L"temporaryDndOwned";
constexpr wchar_t kPersistOriginalProfile[] = L"temporaryDndOriginalProfile";
constexpr wchar_t kPersistTargetProfile[] = L"temporaryDndTargetProfile";

bool IsKnownProfile(const std::wstring& profile) {
    return profile == kProfileUnrestricted ||
           profile == kProfilePriorityOnly ||
           profile == kProfileAlarmsOnly;
}

std::wstring ReadPersistentString(const wchar_t* name) {
    wchar_t buffer[256] = {};
    if (!Wh_GetStringValue(name, buffer, ARRAYSIZE(buffer))) {
        return L"";
    }
    return buffer;
}

void ClearOwnershipMarker() {
    Wh_SetIntValue(kPersistOwnsDnd, 0);
    Wh_SetStringValue(kPersistOriginalProfile, L"");
    Wh_SetStringValue(kPersistTargetProfile, L"");
}

void SaveOwnershipMarker(const std::wstring& original,
                         const std::wstring& target) {
    // Store the strings before arming the marker. If the process dies in the
    // middle, an unarmed partial marker is ignored on next start.
    Wh_SetStringValue(kPersistOriginalProfile, original.c_str());
    Wh_SetStringValue(kPersistTargetProfile, target.c_str());
    Wh_SetIntValue(kPersistOwnsDnd, 1);
}

HRESULT CreateQuietHoursSettings(IQuietHoursSettings** settings) {
    *settings = nullptr;
    return CoCreateInstance(kClsidQuietHoursSettings, nullptr,
                            CLSCTX_LOCAL_SERVER, kIidQuietHoursSettings,
                            reinterpret_cast<void**>(settings));
}

bool GetUserSelectedProfile(std::wstring* profile) {
    IQuietHoursSettings* settings = nullptr;
    HRESULT hr = CreateQuietHoursSettings(&settings);
    if (FAILED(hr) || !settings) {
        Wh_Log(L"QuietHours CoCreateInstance failed: 0x%08X", hr);
        return false;
    }

    LPWSTR rawProfile = nullptr;
    hr = settings->get_UserSelectedProfile(&rawProfile);
    if (SUCCEEDED(hr) && rawProfile) {
        *profile = rawProfile;
    }

    if (rawProfile) {
        CoTaskMemFree(rawProfile);
    }
    settings->Release();

    if (FAILED(hr)) {
        Wh_Log(L"get_UserSelectedProfile failed: 0x%08X", hr);
        return false;
    }

    Wh_Log(L"Current DND profile: %s", profile->c_str());
    return true;
}

bool SetUserSelectedProfile(const std::wstring& profile) {
    IQuietHoursSettings* settings = nullptr;
    HRESULT hr = CreateQuietHoursSettings(&settings);
    if (FAILED(hr) || !settings) {
        Wh_Log(L"QuietHours CoCreateInstance failed: 0x%08X", hr);
        return false;
    }

    hr = settings->put_UserSelectedProfile(
        const_cast<LPWSTR>(profile.c_str()));
    settings->Release();

    if (FAILED(hr)) {
        Wh_Log(L"put_UserSelectedProfile(%s) failed: 0x%08X",
               profile.c_str(), hr);
        return false;
    }

    Wh_Log(L"DND profile set to: %s", profile.c_str());
    return true;
}

std::wstring SelectedSuppressionProfile() {
    return g_settings.suppressionProfile.load() == SuppressionProfile::Alarms
               ? kProfileAlarmsOnly
               : kProfilePriorityOnly;
}

bool AwayConditionActive() {
    return (g_settings.suppressOnDisplayOff.load() && g_displayOff) ||
           (g_settings.suppressOnLock.load() && g_sessionLocked) ||
           (g_settings.suppressOnSuspend.load() && g_suspended);
}

void CancelPendingRestore(HWND hwnd) {
    KillTimer(hwnd, kRestoreTimerId);
}

void AcquireTemporaryDnd(HWND hwnd) {
    CancelPendingRestore(hwnd);

    if (g_ownsTemporaryDnd) {
        return;
    }

    std::wstring current;
    if (!GetUserSelectedProfile(&current)) {
        Wh_Log(L"Could not read DND state; leaving it untouched.");
        return;
    }

    // Do not second-guess the user. If DND was already enabled, the mod owns
    // nothing and therefore must never turn it off later.
    if (current != kProfileUnrestricted) {
        Wh_Log(L"DND was already active (%s); preserving user state.",
               current.c_str());
        return;
    }

    std::wstring target = SelectedSuppressionProfile();
    SaveOwnershipMarker(current, target);

    if (!SetUserSelectedProfile(target)) {
        ClearOwnershipMarker();
        return;
    }

    g_originalProfile = current;
    g_targetProfile = target;
    g_ownsTemporaryDnd = true;
    Wh_Log(L"Temporary DND acquired.");
}

void ReleaseTemporaryDndIfOwned() {
    if (!g_ownsTemporaryDnd) {
        return;
    }

    std::wstring current;
    if (!GetUserSelectedProfile(&current)) {
        // Keep the ownership marker armed. A later retry or the next mod start
        // can recover it rather than silently leaving DND stuck.
        Wh_Log(L"Could not read DND state during restore; keeping ownership marker.");
        return;
    }

    if (current == g_targetProfile) {
        if (!SetUserSelectedProfile(g_originalProfile)) {
            Wh_Log(L"Restore failed; keeping ownership marker for recovery.");
            return;
        }
        Wh_Log(L"Previous DND profile restored: %s", g_originalProfile.c_str());
    } else {
        // Something (presumably the user or Windows) changed the profile while
        // we were away. Never overwrite that newer choice.
        Wh_Log(L"DND changed externally to %s; not restoring over it.",
               current.c_str());
    }

    ClearOwnershipMarker();
    g_ownsTemporaryDnd = false;
    g_originalProfile.clear();
    g_targetProfile.clear();
}

void RecoverStaleOwnership() {
    if (Wh_GetIntValue(kPersistOwnsDnd, 0) == 0) {
        return;
    }

    std::wstring original = ReadPersistentString(kPersistOriginalProfile);
    std::wstring target = ReadPersistentString(kPersistTargetProfile);

    Wh_Log(L"Found stale temporary-DND ownership marker.");

    if (!IsKnownProfile(original) || !IsKnownProfile(target)) {
        Wh_Log(L"Stale marker is malformed; clearing it without changing DND.");
        ClearOwnershipMarker();
        return;
    }

    std::wstring current;
    if (!GetUserSelectedProfile(&current)) {
        Wh_Log(L"Cannot inspect stale state yet; leaving marker armed.");
        return;
    }

    if (current == target) {
        if (!SetUserSelectedProfile(original)) {
            Wh_Log(L"Failed to recover stale DND state; marker remains armed.");
            return;
        }
        Wh_Log(L"Recovered stale DND state to: %s", original.c_str());
    } else {
        Wh_Log(L"DND no longer matches stale target; not overwriting current state.");
    }

    ClearOwnershipMarker();
}

void ReevaluateSuppression(HWND hwnd) {
    if (AwayConditionActive()) {
        AcquireTemporaryDnd(hwnd);
        return;
    }

    if (!g_ownsTemporaryDnd) {
        return;
    }

    DWORD delay = g_settings.restoreDelayMs.load();
    delay = std::clamp<DWORD>(delay, 0, 60000);

    CancelPendingRestore(hwnd);
    if (delay == 0) {
        ReleaseTemporaryDndIfOwned();
    } else {
        SetTimer(hwnd, kRestoreTimerId, delay, nullptr);
        Wh_Log(L"Active again; DND restore scheduled in %u ms.", delay);
    }
}

void LoadSettings() {
    int restoreDelay = Wh_GetIntSetting(L"restoreDelayMs");
    restoreDelay = std::clamp(restoreDelay, 0, 60000);
    g_settings.restoreDelayMs.store(static_cast<DWORD>(restoreDelay));

    auto profileSetting =
        WindhawkUtils::StringSetting::make(L"suppressionProfile");
    std::wstring profile = profileSetting.get();
    g_settings.suppressionProfile.store(
        profile == L"alarms" ? SuppressionProfile::Alarms
                             : SuppressionProfile::Priority);

    g_settings.suppressOnDisplayOff.store(
        Wh_GetIntSetting(L"suppressOnDisplayOff") != 0);
    g_settings.suppressOnLock.store(
        Wh_GetIntSetting(L"suppressOnLock") != 0);
    g_settings.suppressOnSuspend.store(
        Wh_GetIntSetting(L"suppressOnSuspend") != 0);
}

LRESULT CALLBACK QuietWindowProc(HWND hwnd,
                                 UINT msg,
                                 WPARAM wParam,
                                 LPARAM lParam) {
    switch (msg) {
        case WM_POWERBROADCAST: {
            if (wParam == PBT_POWERSETTINGCHANGE) {
                auto* pbs = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                if (pbs && pbs->PowerSetting == kGuidSessionDisplayStatus &&
                    pbs->DataLength >= sizeof(DWORD)) {
                    DWORD state = 0;
                    std::memcpy(&state, pbs->Data, sizeof(state));

                    if (state == 0) {  // PowerMonitorOff
                        g_displayOff = true;
                        Wh_Log(L"Display state: OFF");
                    } else if (state == 1) {  // PowerMonitorOn
                        g_displayOff = false;
                        Wh_Log(L"Display state: ON");
                    } else {  // Dimmed: still treat the user session as active.
                        g_displayOff = false;
                        Wh_Log(L"Display state: DIMMED");
                    }
                    ReevaluateSuppression(hwnd);
                }
            } else if (wParam == PBT_APMSUSPEND) {
                g_suspended = true;
                Wh_Log(L"System suspending.");
                ReevaluateSuppression(hwnd);
            } else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                       wParam == PBT_APMRESUMESUSPEND) {
                g_suspended = false;
                Wh_Log(L"System resumed.");
                ReevaluateSuppression(hwnd);
            }
            return TRUE;
        }

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) {
                g_sessionLocked = true;
                Wh_Log(L"Session locked.");
                ReevaluateSuppression(hwnd);
            } else if (wParam == WTS_SESSION_UNLOCK) {
                g_sessionLocked = false;
                Wh_Log(L"Session unlocked.");
                ReevaluateSuppression(hwnd);
            }
            return 0;

        case WM_TIMER:
            if (wParam == kRestoreTimerId) {
                CancelPendingRestore(hwnd);
                if (!AwayConditionActive()) {
                    ReleaseTemporaryDndIfOwned();
                }
            }
            return 0;

        case kMsgSettingsChanged:
            ReevaluateSuppression(hwnd);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI NotificationGuardThread(LPVOID) {
    Wh_Log(L"Notification guard worker started.");

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        Wh_Log(L"CoInitializeEx failed: 0x%08X", comHr);
        SetEvent(g_readyEvent);
        return 1;
    }

    // A prior process crash or forced Windhawk shutdown must not leave the
    // temporary DND profile permanently selected.
    RecoverStaleOwnership();

    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = QuietWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkQuietNotificationsWhileAway";

    if (!RegisterClassExW(&wc)) {
        Wh_Log(L"RegisterClassExW failed: %u", GetLastError());
        SetEvent(g_readyEvent);
        if (comInitialized) {
            CoUninitialize();
        }
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"WindhawkQuietNotificationsWhileAway", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        Wh_Log(L"CreateWindowExW failed: %u", GetLastError());
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        SetEvent(g_readyEvent);
        if (comInitialized) {
            CoUninitialize();
        }
        return 1;
    }

    HPOWERNOTIFY powerNotify = RegisterPowerSettingNotification(
        hwnd, &kGuidSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!powerNotify) {
        Wh_Log(L"RegisterPowerSettingNotification failed: %u", GetLastError());
    }

    bool wtsRegistered =
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!wtsRegistered) {
        Wh_Log(L"WTSRegisterSessionNotification failed: %u", GetLastError());
    }

    SetEvent(g_readyEvent);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Settings-change notifications are posted to the worker thread rather
        // than to a window handle, so handle them directly here.
        if (!msg.hwnd && msg.message == kMsgSettingsChanged) {
            ReevaluateSuppression(hwnd);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // If the mod is disabled/reloaded while it owns DND, undo only our own
    // temporary change before exiting.
    CancelPendingRestore(hwnd);
    ReleaseTemporaryDndIfOwned();

    if (wtsRegistered) {
        WTSUnRegisterSessionNotification(hwnd);
    }
    if (powerNotify) {
        UnregisterPowerSettingNotification(powerNotify);
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (comInitialized) {
        CoUninitialize();
    }

    Wh_Log(L"Notification guard worker exited.");
    return 0;
}

BOOL WhTool_ModInit() {
    Wh_Log(L"Init");
    LoadSettings();

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_readyEvent) {
        Wh_Log(L"CreateEventW failed: %u", GetLastError());
        return FALSE;
    }

    g_thread =
        CreateThread(nullptr, 0, NotificationGuardThread, nullptr, 0, &g_threadId);
    if (!g_thread) {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
        return FALSE;
    }

    return TRUE;
}

void WhTool_ModSettingsChanged() {
    Wh_Log(L"Settings changed.");
    LoadSettings();

    if (g_threadId) {
        PostThreadMessageW(g_threadId, kMsgSettingsChanged, 0, 0);
    }
}

void WhTool_ModUninit() {
    Wh_Log(L"Uninit");

    if (g_thread && g_threadId) {
        if (g_readyEvent) {
            WaitForSingleObject(g_readyEvent, INFINITE);
        }
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_threadId = 0;
    }

    if (g_readyEvent) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
    }
}

}  // namespace

// clang-format off
// ============================================================================
// Windhawk tool-mod boilerplate
// Runs the helper in a Windhawk-owned process instead of injecting persistent
// background code into Explorer or another application.
// ============================================================================
bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            reinterpret_cast<IMAGE_DOS_HEADER*>(GetModuleHandle(nullptr));
        IMAGE_NT_HEADERS* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<BYTE*>(dosHeader) + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = reinterpret_cast<BYTE*>(dosHeader) + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, reinterpret_cast<void*>(EntryPoint_Hook),
                           nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR commandLine[
        MAX_PATH + 2 +
        (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);

    auto pCreateProcessInternalW =
        reinterpret_cast<CreateProcessInternalW_t>(GetProcAddress(
            kernelModule, "CreateProcessInternalW"));
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi{};
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
// clang-format on