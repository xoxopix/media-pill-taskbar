// ==WindhawkMod==
// @id              media-pill-taskbar
// @name            Media Pill Taskbar Widget
// @description     Compact Apple-style media pill embedded natively inside the Windows 11 taskbar tray (album art background, circular progress ring, Play/Pause & Next, SMTC + AIMP support).
// @version         1.0.0
// @author          EVA 01
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -luuid -luser32 -lwindowsapp -lshell32 -lshlwapi -lshcore
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Media Pill Taskbar Widget
Embeds a compact pill media controller natively inside the Windows 11 taskbar (`SystemTrayFrameGrid`).
- Blurred/tinted album cover fills the pill background
- Circular progress arc around the Play/Pause button (supports SMTC + AIMP playlist duration)
- Next track button
- Moves and auto-hides natively at 100% framerate with the taskbar
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideWhenIdle: false
  $name: Ocultar si no hay reproducción
  $description: Oculta la cápsula cuando no hay ninguna sesión multimedia activa.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <initguid.h>
#include <knownfolders.h>
#include <ocidl.h>
#include <roapi.h>
#include <shcore.h>
#include <shellapi.h>
#include <propkey.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <unknwn.h>
#include <xamlom.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
namespace wuco = winrt::Windows::UI::Composition;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
namespace wuxi = winrt::Windows::UI::Xaml::Input;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxmi = winrt::Windows::UI::Xaml::Media::Imaging;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;

// ---------------------------------------------------------------------------
// Global state & settings
// ---------------------------------------------------------------------------
static struct {
    bool hideWhenIdle = false;
} g_settings;

static void DebugLog(const wchar_t*, bool = false) {}

static std::atomic<bool> g_unloading{false};
static std::atomic<bool> g_initialized{false};
static thread_local bool g_initializedForThread = false;
static std::atomic<bool> g_reportCompositionDiagAsDisabled{false};
static DWORD g_taskbarUiThreadId = 0;

// ---------------------------------------------------------------------------
// Module handle & UI thread dispatch helpers
// ---------------------------------------------------------------------------
static HMODULE GetCurrentModuleHandle() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle),
                            &module)) {
        return nullptr;
    }
    return module;
}

static HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId = 0;
            WCHAR className[32]{};
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));
    return hTaskbarWnd;
}

static HWND GetTaskbarUiWnd() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) return nullptr;
    return FindWindowExW(hTaskbarWnd, nullptr,
                         L"Windows.UI.Composition.DesktopWindowContentBridge",
                         nullptr);
}

using RunFromWindowThreadProc_t = void(WINAPI*)(PVOID parameter);

static bool RunFromWindowThread(HWND hWnd,
                                RunFromWindowThreadProc_t proc,
                                PVOID procParam) {
    static const UINT runMsg =
        RegisterWindowMessageW(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct PARAM {
        RunFromWindowThreadProc_t proc;
        PVOID procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) return false;
    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == runMsg) {
                    auto* p = reinterpret_cast<PARAM*>(cwp->lParam);
                    p->proc(p->procParam);
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) return false;

    PARAM param{proc, procParam};
    SendMessageW(hWnd, runMsg, 0, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return true;
}

static HWND FindThreadWindow(DWORD threadId) {
    HWND hWnd = nullptr;
    EnumThreadWindows(
        threadId,
        [](HWND w, LPARAM lParam) -> BOOL {
            *reinterpret_cast<HWND*>(lParam) = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&hWnd));
    if (hWnd) return hWnd;

    while ((hWnd = FindWindowExW(HWND_MESSAGE, hWnd, nullptr, nullptr))) {
        if (GetWindowThreadProcessId(hWnd, nullptr) == threadId) return hWnd;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Media state shared between SMTC worker thread and Taskbar UI thread
// ---------------------------------------------------------------------------
struct MediaSnapshot {
    std::wstring appId;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    bool hasSession = false;
    bool isPlaying = false;
    bool hasNativeTimeline = false;
    double positionSec = 0.0;
    double durationSec = 0.0;
    std::chrono::steady_clock::time_point capturedAt{};
    uint64_t thumbVersion = 0;
    std::vector<uint8_t> thumbBytes;
};

static std::mutex g_mediaMtx;
static MediaSnapshot g_mediaState;
static uint64_t g_thumbCounter = 0;
static std::wstring g_lastTrackKey;
static std::wstring g_lastAimpTrackKey;
static double g_aimpDurationSec = 0.0;
static double g_aimpElapsedSec = 0.0;
static auto g_lastMediaPoll = std::chrono::steady_clock::now();

static std::atomic<int> g_pendingAction{0};  // 1=play/pause, 2=next
static std::thread g_mediaThread;
static HANDLE g_mediaStopEvent = nullptr;
static HANDLE g_mediaActionEvent = nullptr;

// ---------------------------------------------------------------------------
// UI elements owned by the Taskbar UI thread
// ---------------------------------------------------------------------------
static wuxc::Panel g_injectedParent{nullptr};
static wuxc::Grid g_pillContainer{nullptr};
static wuxc::Canvas g_artHost{nullptr};
static wuxc::StackPanel g_controlsPanel{nullptr};
static wuxs::Path g_progressArc{nullptr};
static wuxm::ArcSegment g_arcSeg{nullptr};
static wuxs::Path g_iconPlay{nullptr};
static wuxs::Path g_iconPause{nullptr};
static wuxc::MenuFlyoutItem g_flyoutTrackItem{nullptr};
static wux::DispatcherTimer g_uiTimer{nullptr};
static winrt::event_token g_uiTimerToken{};
static uint64_t g_uiThumbVersion = 0;
static bool g_injectedColumnCreated = false;
static bool g_pillHovered = false;
static bool g_uiHasArt = false;
static bool g_uiIsPlaying = false;
static bool g_controlsVisible = true;

// ---------------------------------------------------------------------------
// XAML Diagnostics / VisualTreeWatcher
// ---------------------------------------------------------------------------
static constexpr GUID IID_IXamlDiagnosticsTestHooks = {
    0x735941a2, 0x3ee3, 0x495a, {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(
        InstanceHandle handle, void** dispatcherQueue) = 0;
};

static constexpr CLSID CLSID_WindhawkMediaPillTAP = {
    0x9b7e2f41, 0x3c18, 0x4d92, {0x9a, 0x11, 0x44, 0x20, 0x8b, 0x7a, 0x55, 0xc3}};

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site);
    ~VisualTreeWatcher() = default;

    void UnadviseVisualTreeChange();
    std::vector<DWORD> GetCallbackThreadIds();

   private:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) override;
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle element,
        VisualElementState elementState,
        LPCWSTR context) noexcept override;

    void RecordCallbackThread();
    void ReleaseHandle(InstanceHandle handle);
    wf::IInspectable FromHandle(InstanceHandle handle);

    winrt::com_ptr<IXamlDiagnostics> m_xamlDiagnostics;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> m_testHooks;
    winrt::slim_mutex m_threadLock;
    std::vector<DWORD> m_callbackThreadIds;
};

[[clang::no_destroy]] static winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;
static SRWLOCK g_watcherLock = SRWLOCK_INIT;

static winrt::com_ptr<VisualTreeWatcher> ExchangeWatcher(
    winrt::com_ptr<VisualTreeWatcher> next) {
    AcquireSRWLockExclusive(&g_watcherLock);
    std::swap(g_visualTreeWatcher, next);
    ReleaseSRWLockExclusive(&g_watcherLock);
    return next;
}

static bool IsKnownMediaApp(std::wstring const& appId) {
    return appId.find(L"AIMP") != std::wstring::npos ||
           appId.find(L"aimp") != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// AIMP playlist duration resolver (runs on worker thread)
// ---------------------------------------------------------------------------
static double ResolveAimpDurationFromPlaylist(std::wstring const& title) {
    if (title.empty()) return 210.0;
    try {
        PWSTR appDataPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
            std::filesystem::path plsDir = std::filesystem::path(appDataPath) / L"AIMP" / L"PLS";
            CoTaskMemFree(appDataPath);

            if (std::filesystem::exists(plsDir)) {
                std::vector<std::filesystem::directory_entry> files;
                for (auto const& entry : std::filesystem::directory_iterator(plsDir)) {
                    if (entry.is_regular_file() && entry.path().extension() == L".aimppl4") {
                        files.push_back(entry);
                    }
                }
                std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) {
                    return a.last_write_time() > b.last_write_time();
                });

                std::string utf8Title;
                int len = WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (len > 1) {
                    utf8Title.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, utf8Title.data(), len, nullptr, nullptr);
                }

                // Case-insensitive ascii/utf8 substring check
                auto icontains = [](std::string const& hay, std::string const& needle) {
                    if (needle.empty()) return false;
                    auto it = std::search(
                        hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](unsigned char a, unsigned char b) {
                            return std::tolower(a) == std::tolower(b);
                        });
                    return it != hay.end();
                };

                size_t maxFiles = std::min<size_t>(files.size(), 8);
                for (size_t i = 0; i < maxFiles; ++i) {
                    std::ifstream in(files[i].path());
                    std::string line;
                    while (std::getline(in, line)) {
                        if (!utf8Title.empty() && icontains(line, utf8Title)) {
                            // Field 15 separated by '|' is duration in ms
                            size_t pos = 0;
                            int fieldIdx = 0;
                            while (fieldIdx < 15) {
                                size_t next = line.find('|', pos);
                                if (next == std::string::npos) {
                                    pos = std::string::npos;
                                    break;
                                }
                                pos = next + 1;
                                fieldIdx++;
                            }
                            if (pos != std::string::npos) {
                                size_t end = line.find('|', pos);
                                std::string durStr = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                                try {
                                    long long durMs = std::stoll(durStr);
                                    if (durMs > 1000) return durMs / 1000.0;
                                } catch (...) {}
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {}

    return 210.0;
}

// ---------------------------------------------------------------------------
// SMTC background thread
// ---------------------------------------------------------------------------
static wmc::GlobalSystemMediaTransportControlsSession PickActiveSession(
    wmc::GlobalSystemMediaTransportControlsSessionManager const& mgr,
    std::wstring const& preferredAppId) {
    try {
        auto sessions = mgr.GetSessions();
        for (auto const& s : sessions) {
            auto info = s.GetPlaybackInfo();
            if (info && info.PlaybackStatus() ==
                            wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                return s;
            }
        }
        if (!preferredAppId.empty()) {
            for (auto const& s : sessions) {
                if (std::wstring_view(s.SourceAppUserModelId()) == preferredAppId) {
                    return s;
                }
            }
        }
        return mgr.GetCurrentSession();
    } catch (...) {
        return nullptr;
    }
}

static std::vector<uint8_t> ReadThumbnailBytes(
    wss::IRandomAccessStreamReference const& thumbRef) {
    std::vector<uint8_t> bytes;
    if (!thumbRef) return bytes;
    try {
        auto stream = thumbRef.OpenReadAsync().get();
        if (!stream) return bytes;
        stream.Seek(0);
        constexpr uint64_t kMaxThumbBytes = 32ULL * 1024 * 1024;  // handles 3000px+ lossless PNGs
        uint64_t size = stream.Size();
        if (size == 0 || size > kMaxThumbBytes) size = kMaxThumbBytes;
        bytes.reserve(static_cast<size_t>(std::min<uint64_t>(size, 16ULL * 1024 * 1024)));

        wss::Buffer buf(65536);
        while (bytes.size() < size) {
            auto readBuf = stream.ReadAsync(buf, 65536, wss::InputStreamOptions::Partial).get();
            uint32_t actual = readBuf.Length();
            if (actual == 0) break;
            size_t oldSize = bytes.size();
            bytes.resize(oldSize + actual);
            std::memcpy(bytes.data() + oldSize, readBuf.data(), actual);
        }
    } catch (...) {
        WCHAR buf[128];
        wsprintfW(buf, L"ReadThumbnailBytes EXCEPTION hr=0x%08X", winrt::to_hresult());
        DebugLog(buf);
    }
    return bytes;
}

static void QueryAimpLiveProgress(
    bool isAimp,
    std::wstring const& title,
    std::wstring const& artist,
    bool playing,
    double dt,
    double& outPosSec,
    double& outDurSec) {
    // AIMP2_RemoteInfo MMF header (uint32[4] = duration ms when active)
    double mmfDurSec = 0.0;
    if (isAimp) {
        if (HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"AIMP2_RemoteInfo")) {
            if (const auto* hdr = static_cast<const uint32_t*>(
                    MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(uint32_t) * 8))) {
                if (hdr[0] >= 20 && hdr[1] != 0 && hdr[4] > 0) {
                    mmfDurSec = static_cast<double>(hdr[4]) / 1000.0;
                }
                UnmapViewOfFile(hdr);
            }
            CloseHandle(hMap);
        }
    }

    std::wstring aimpKey = title + L"|" + artist;
    if (aimpKey != g_lastAimpTrackKey) {
        g_lastAimpTrackKey = aimpKey;
        g_aimpElapsedSec = 0.0;
        g_aimpDurationSec = !isAimp ? 0.0
                                    : ((mmfDurSec > 0.0) ? mmfDurSec : ResolveAimpDurationFromPlaylist(title));
    } else if (mmfDurSec > 0.0) {
        g_aimpDurationSec = mmfDurSec;
    }

    bool gotLivePos = false;
    if (isAimp) {
        HWND aimpWnd = FindWindowW(L"AIMP2_RemoteInfo", nullptr);
        if (!aimpWnd) aimpWnd = FindWindowExW(HWND_MESSAGE, nullptr, L"AIMP2_RemoteInfo", nullptr);
        if (aimpWnd) {
            DWORD_PTR posMs = 0;
            constexpr UINT WM_AIMP_PROPERTY = WM_USER + 0x77;
            constexpr WPARAM AIMP_RA_PROPERTY_PLAYER_POSITION_GET = 0x20;
            if (SendMessageTimeoutW(aimpWnd, WM_AIMP_PROPERTY, AIMP_RA_PROPERTY_PLAYER_POSITION_GET,
                                    0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &posMs)) {
                g_aimpElapsedSec = static_cast<double>(posMs) / 1000.0;
                gotLivePos = true;
            }
        }
    }

    if (!gotLivePos && playing && dt > 0.0 && dt < 2.0) {
        g_aimpElapsedSec += dt;
    }

    outDurSec = g_aimpDurationSec;
    outPosSec = (outDurSec > 0.0) ? std::clamp(g_aimpElapsedSec, 0.0, outDurSec) : g_aimpElapsedSec;
}

static bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

static bool LaunchShellNoUi(LPCWSTR target) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = target;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

static bool FocusProcessByExeName(std::wstring const& exeName) {
    if (exeName.empty()) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::vector<DWORD> pids;
    std::wstring exePath;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (EqualsIgnoreCase(pe.szExeFile, exeName)) {
                pids.push_back(pe.th32ProcessID);
                if (exePath.empty()) {
                    if (HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                        WCHAR pathBuf[MAX_PATH];
                        DWORD pathLen = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
                            exePath.assign(pathBuf, pathLen);
                        }
                        CloseHandle(hProc);
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (pids.empty()) return false;

    struct EnumCtx {
        std::vector<DWORD> const* pids;
        HWND found = nullptr;
        DWORD foundPid = 0;
    } ctx{&pids};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lp);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
                (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            RECT rc{};
            if (!GetWindowRect(hwnd, &rc) || (rc.right - rc.left) < 50 || (rc.bottom - rc.top) < 50) {
                return TRUE;
            }
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (std::find(c->pids->begin(), c->pids->end(), wpid) != c->pids->end()) {
                c->found = hwnd;
                c->foundPid = wpid;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) {
        AllowSetForegroundWindow(ctx.foundPid);
        if (IsIconic(ctx.found)) ShowWindow(ctx.found, SW_RESTORE);
        SetForegroundWindow(ctx.found);
        if (GetForegroundWindow() != ctx.found) {
            SwitchToThisWindow(ctx.found, TRUE);
        }
        return true;
    }

    // Fallback when player is minimized to tray (e.g. AIMP) and has no visible window
    return !exePath.empty() && LaunchShellNoUi(exePath.c_str());
}

static bool ActivateUwpAppId(std::wstring const& appId) {
    winrt::com_ptr<IApplicationActivationManager> aam;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                  IID_PPV_ARGS(aam.put()));
    if (SUCCEEDED(hr) && aam) {
        DWORD pid = 0;
        if (SUCCEEDED(aam->ActivateApplication(appId.c_str(), nullptr, AO_NONE, &pid))) {
            return true;
        }
    }
    return false;
}

static void FocusPlayerByAppId(std::wstring const& appId) {
    if (appId.empty()) return;

    // 1. Store/UWP AUMIDs contain '!' (Spotify Store, Apple Music, Media Player)
    if (appId.find(L'!') != std::wstring::npos && ActivateUwpAppId(appId)) {
        return;
    }

    // 2. Desktop Win32 SMTC sources reporting exe names (AIMP.exe, vlc.exe, chrome.exe)
    size_t slash = appId.find_last_of(L"\\/");
    std::wstring candidate = (slash != std::wstring::npos) ? appId.substr(slash + 1) : appId;
    if (FocusProcessByExeName(candidate)) return;
    if (candidate.find(L'.') == std::wstring::npos &&
        FocusProcessByExeName(candidate + L".exe")) {
        return;
    }

    // 3. Resolve AppsFolder shortcut AUMIDs (e.g. Zen F0DC299D809B9700, Firefox, Edge, Electron)
    winrt::com_ptr<IShellItem2> item;
    std::wstring parseName = L"shell:AppsFolder\\" + appId;
    if (SUCCEEDED(SHCreateItemFromParsingName(parseName.c_str(), nullptr, IID_PPV_ARGS(item.put()))) && item) {
        PWSTR rawTarget = nullptr;
        if (SUCCEEDED(item->GetString(PKEY_Link_TargetParsingPath, &rawTarget)) && rawTarget) {
            std::wstring target(rawTarget);
            CoTaskMemFree(rawTarget);
            size_t tSlash = target.find_last_of(L"\\/");
            std::wstring targetExe = (tSlash != std::wstring::npos) ? target.substr(tSlash + 1) : target;
            if (FocusProcessByExeName(targetExe) || LaunchShellNoUi(target.c_str())) {
                return;
            }
        }
        if (LaunchShellNoUi(parseName.c_str())) return;
    }

    // 4. Last-resort fallbacks
    if (ActivateUwpAppId(appId)) return;
    if (IsKnownMediaApp(appId)) {
        LaunchShellNoUi(L"C:\\Program Files\\AIMP\\AIMP.exe");
    } else if (appId.find(L"Spotify") != std::wstring::npos ||
               appId.find(L"spotify") != std::wstring::npos) {
        LaunchShellNoUi(L"spotify:");
    }
}

static void MediaWorkerProc() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    wmc::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    try {
        mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (...) {}

    std::wstring preferredAppId;
    bool pendingThumbRecheck = false;
    auto thumbRecheckAt = std::chrono::steady_clock::now();
    HANDLE waitHandles[2] = {g_mediaStopEvent, g_mediaActionEvent};

    while (!g_unloading) {
        if (mgr) {
            try {
                auto session = PickActiveSession(mgr, preferredAppId);

                int act = g_pendingAction.exchange(0);
                if (act != 0 && session) {
                    try {
                        auto info = session.GetPlaybackInfo();
                        bool playing = info && info.PlaybackStatus() ==
                                                   wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        if (act == 1) {
                            if (playing) session.TryPauseAsync().get();
                            else session.TryPlayAsync().get();
                        } else if (act == 2) {
                            g_aimpElapsedSec = 0.0;
                            session.TrySkipNextAsync().get();
                        } else if (act == 3) {
                            g_aimpElapsedSec = 0.0;
                            session.TrySkipPreviousAsync().get();
                        }
                    } catch (...) {}
                }

                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - g_lastMediaPoll).count();
                g_lastMediaPoll = now;

                if (session) {
                    preferredAppId = session.SourceAppUserModelId().c_str();
                    auto info = session.GetPlaybackInfo();
                    bool playing = info && info.PlaybackStatus() ==
                                               wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

                    auto props = session.TryGetMediaPropertiesAsync().get();
                    std::wstring title = props ? std::wstring(props.Title()) : L"";
                    std::wstring artist = props ? std::wstring(props.Artist()) : L"";
                    std::wstring album = props ? std::wstring(props.AlbumTitle()) : L"";
                    std::wstring trackKey = preferredAppId + L"|" + title + L"|" + artist;

                    bool trackChanged = (trackKey != g_lastTrackKey);
                    if (trackChanged) {
                        g_lastTrackKey = trackKey;
                        pendingThumbRecheck = true;
                        thumbRecheckAt = now + std::chrono::milliseconds(450);
                    }
                    bool needThumb = trackChanged;
                    if (!needThumb && pendingThumbRecheck && now >= thumbRecheckAt) {
                        needThumb = true;
                        pendingThumbRecheck = false;
                    }
                    if (!needThumb) {
                        std::lock_guard<std::mutex> lk(g_mediaMtx);
                        needThumb = g_mediaState.thumbBytes.empty();
                    }
                    std::vector<uint8_t> newThumb;
                    bool thumbUpdated = false;
                    if (needThumb && props && props.Thumbnail()) {
                        newThumb = ReadThumbnailBytes(props.Thumbnail());
                        if (trackChanged) {
                            thumbUpdated = true;
                        } else if (!newThumb.empty()) {
                            std::lock_guard<std::mutex> lk(g_mediaMtx);
                            thumbUpdated = (newThumb != g_mediaState.thumbBytes);
                        }
                    } else if (trackChanged) {
                        thumbUpdated = true;
                    }

                    bool isAimp = IsKnownMediaApp(preferredAppId);
                    bool hasNativeTimeline = false;
                    double posSec = 0.0;
                    double durSec = 0.0;

                    auto tl = session.GetTimelineProperties();
                    if (!isAimp && tl) {
                        double startS = std::chrono::duration<double>(tl.StartTime()).count();
                        double endS = std::chrono::duration<double>(tl.EndTime()).count();
                        double durS = endS - startS;
                        if (durS > 1.0) {
                            hasNativeTimeline = true;
                            durSec = durS;
                            posSec = std::chrono::duration<double>(tl.Position()).count() - startS;
                            if (playing) {
                                auto lastUpd = tl.LastUpdatedTime();
                                auto age = wf::TimeSpan{wf::DateTime::clock::now().time_since_epoch()} -
                                           wf::TimeSpan{lastUpd.time_since_epoch()};
                                double elapsedS = std::chrono::duration<double>(age).count();
                                if (elapsedS > 0.0) {
                                    posSec += elapsedS;
                                }
                            }
                            posSec = std::clamp(posSec, 0.0, durSec);
                        }
                    }

                    if (!hasNativeTimeline) {
                        QueryAimpLiveProgress(isAimp, title, artist, playing, dt, posSec, durSec);
                    }

                    {
                        std::lock_guard<std::mutex> lk(g_mediaMtx);
                        g_mediaState.hasSession = true;
                        g_mediaState.appId = preferredAppId;
                        g_mediaState.title = std::move(title);
                        g_mediaState.artist = std::move(artist);
                        g_mediaState.album = std::move(album);
                        g_mediaState.isPlaying = playing;
                        g_mediaState.hasNativeTimeline = hasNativeTimeline;
                        g_mediaState.positionSec = posSec;
                        g_mediaState.durationSec = durSec;
                        g_mediaState.capturedAt = now;
                        if (thumbUpdated) {
                            g_mediaState.thumbBytes = std::move(newThumb);
                            g_mediaState.thumbVersion = ++g_thumbCounter;
                        }
                    }
                } else {
                    std::lock_guard<std::mutex> lk(g_mediaMtx);
                    if (g_mediaState.hasSession) {
                        g_mediaState = MediaSnapshot{};
                        g_mediaState.thumbVersion = ++g_thumbCounter;
                        g_lastTrackKey.clear();
                        g_lastAimpTrackKey.clear();
                    }
                }
            } catch (...) {}
        }

        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, 250);
        if (waitRes == WAIT_OBJECT_0) break;  // stop event
    }

    mgr = nullptr;
    winrt::uninit_apartment();
}

// ---------------------------------------------------------------------------
// XAML UI construction & live update (Taskbar UI thread only)
// ---------------------------------------------------------------------------
static wuxm::Geometry MakePolyGeometry(
    std::initializer_list<std::initializer_list<wf::Point>> polys) {
    wuxm::PathGeometry geo;
    for (auto const& pts : polys) {
        if (pts.size() == 0) continue;
        wuxm::PathFigure fig;
        fig.IsClosed(true);
        fig.IsFilled(true);
        bool first = true;
        for (auto const& pt : pts) {
            if (first) {
                fig.StartPoint(pt);
                first = false;
            } else {
                wuxm::LineSegment seg;
                seg.Point(pt);
                fig.Segments().Append(seg);
            }
        }
        geo.Figures().Append(fig);
    }
    return geo;
}

static void SetProgressArc(double progress) {
    if (!g_progressArc || !g_arcSeg) return;
    if (progress <= 0.002) {
        g_progressArc.Opacity(0.0);
        return;
    }
    g_progressArc.Opacity(1.0);
    double clamped = std::clamp(progress, 0.0, 0.9985);
    double angle = clamped * 360.0;
    double rad = (angle - 90.0) * (3.14159265358979323846 / 180.0);
    constexpr double cx = 12.0;
    constexpr double cy = 12.0;
    constexpr double r = 11.0;
    g_arcSeg.Point({static_cast<float>(cx + r * std::cos(rad)),
                    static_cast<float>(cy + r * std::sin(rad))});
    g_arcSeg.IsLargeArc(angle > 180.0);
}

static void UpdateControlsFadeOnUi(bool animate = true) {
    if (!g_controlsPanel) return;
    bool shouldShow = g_pillHovered || !g_uiHasArt || !g_uiIsPlaying;
    if (animate && shouldShow == g_controlsVisible) return;
    g_controlsVisible = shouldShow;
    g_controlsPanel.IsHitTestVisible(shouldShow);
    float targetOpacity = shouldShow ? 1.0f : 0.0f;
    try {
        auto visual = wuxh::ElementCompositionPreview::GetElementVisual(g_controlsPanel);
        auto comp = visual.Compositor();
        if (!animate) {
            visual.StopAnimation(L"Opacity");
            visual.Opacity(targetOpacity);
            return;
        }
        auto anim = comp.CreateScalarKeyFrameAnimation();
        anim.Duration(std::chrono::milliseconds(shouldShow ? 140 : 200));
        anim.InsertKeyFrame(1.0f, targetOpacity);
        visual.StartAnimation(L"Opacity", anim);
    } catch (...) {
        g_controlsPanel.Opacity(shouldShow ? 1.0 : 0.0);
    }
}

static void UpdateThumbnailOnUi(std::vector<uint8_t> bytes) {
    if (!g_artHost) return;

    if (bytes.empty()) {
        g_artHost.Children().Clear();
        g_uiHasArt = false;
        UpdateControlsFadeOnUi(true);
        return;
    }

    try {
        winrt::com_ptr<IStream> iStream;
        iStream.attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
        if (iStream) {
            wss::IRandomAccessStream raStream{nullptr};
            HRESULT hr = CreateRandomAccessStreamOverStream(
                iStream.get(), BSOS_DEFAULT,
                winrt::guid_of<wss::IRandomAccessStream>(),
                winrt::put_abi(raStream));
            if (SUCCEEDED(hr) && raStream) {
                uint64_t expectedVersion = g_uiThumbVersion;
                wuxc::Image newImg;
                newImg.Width(106.0);
                newImg.Height(106.0);
                newImg.Stretch(wuxm::Stretch::Fill);
                newImg.Opacity(0.0);
                newImg.IsHitTestVisible(false);
                wuxc::Canvas::SetLeft(newImg, 0.0);
                wuxc::Canvas::SetTop(newImg, -36.0);

                wuxmi::BitmapImage bmp;
                bmp.DecodePixelWidth(212);
                bmp.ImageFailed([newImg, expectedVersion](auto const&, auto const&) {
                    if (g_artHost && expectedVersion == g_uiThumbVersion) {
                        uint32_t idx = 0;
                        if (g_artHost.Children().IndexOf(newImg, idx)) {
                            g_artHost.Children().RemoveAt(idx);
                        }
                        if (g_artHost.Children().Size() == 0) {
                            g_uiHasArt = false;
                            UpdateControlsFadeOnUi(true);
                        }
                    }
                });
                bmp.ImageOpened([newImg, expectedVersion](wf::IInspectable const& sender, auto const&) {
                    if (!g_artHost || expectedVersion != g_uiThumbVersion) {
                        if (g_artHost) {
                            uint32_t idx = 0;
                            if (g_artHost.Children().IndexOf(newImg, idx)) {
                                g_artHost.Children().RemoveAt(idx);
                            }
                        }
                        return;
                    }
                    if (auto b = sender.try_as<wuxmi::BitmapImage>()) {
                        int pw = b.PixelWidth();
                        int ph = b.PixelHeight();
                        if (pw > 0 && ph > 0) {
                            double w = 106.0;
                            double h = 106.0 * static_cast<double>(ph) / static_cast<double>(pw);
                            if (h < 34.0) {
                                h = 34.0;
                                w = 34.0 * static_cast<double>(pw) / static_cast<double>(ph);
                            }
                            newImg.Width(w);
                            newImg.Height(h);
                            wuxc::Canvas::SetLeft(newImg, (106.0 - w) / 2.0);
                            wuxc::Canvas::SetTop(newImg, (34.0 - h) / 2.0);
                        }
                    }
                    newImg.Opacity(1.0);
                    for (int i = static_cast<int>(g_artHost.Children().Size()) - 1; i >= 0; --i) {
                        auto child = g_artHost.Children().GetAt(static_cast<uint32_t>(i));
                        if (child != newImg) {
                            g_artHost.Children().RemoveAt(static_cast<uint32_t>(i));
                        }
                    }
                    g_artHost.UpdateLayout();
                    g_uiHasArt = true;
                    UpdateControlsFadeOnUi(true);
                });
                bmp.SetSource(raStream);
                newImg.Source(bmp);
                g_artHost.Children().Append(newImg);
                return;
            }
        }
    } catch (...) {}

    g_artHost.Children().Clear();
    g_uiHasArt = false;
    UpdateControlsFadeOnUi(true);
}

static void RefreshUiFromMediaSnapshot() {
    if (!g_pillContainer || g_unloading) return;

    MediaSnapshot snap;
    bool thumbChanged = false;
    {
        std::lock_guard<std::mutex> lk(g_mediaMtx);
        snap.hasSession = g_mediaState.hasSession;
        snap.appId = g_mediaState.appId;
        snap.title = g_mediaState.title;
        snap.artist = g_mediaState.artist;
        snap.album = g_mediaState.album;
        snap.isPlaying = g_mediaState.isPlaying;
        snap.hasNativeTimeline = g_mediaState.hasNativeTimeline;
        snap.positionSec = g_mediaState.positionSec;
        snap.durationSec = g_mediaState.durationSec;
        snap.capturedAt = g_mediaState.capturedAt;
        if (g_mediaState.thumbVersion != g_uiThumbVersion) {
            thumbChanged = true;
            g_uiThumbVersion = g_mediaState.thumbVersion;
            snap.thumbBytes = g_mediaState.thumbBytes;
        }
    }

    if (g_settings.hideWhenIdle && !snap.hasSession) {
        g_pillContainer.Visibility(wux::Visibility::Collapsed);
    } else {
        g_pillContainer.Visibility(wux::Visibility::Visible);
    }

    if (thumbChanged) {
        UpdateThumbnailOnUi(std::move(snap.thumbBytes));
    }

    if (g_iconPlay && g_iconPause) {
        g_iconPlay.Visibility(snap.isPlaying ? wux::Visibility::Collapsed
                                             : wux::Visibility::Visible);
        g_iconPause.Visibility(snap.isPlaying ? wux::Visibility::Visible
                                              : wux::Visibility::Collapsed);
    }
    g_uiIsPlaying = snap.isPlaying;
    UpdateControlsFadeOnUi(true);

    // Tooltip & flyout label
    std::wstring info = snap.title;
    if (!snap.artist.empty()) {
        if (!info.empty()) info += L" • ";
        info += snap.artist;
    }
    if (info.empty()) info = L"Media Pill Widget";
    wuxc::ToolTipService::SetToolTip(g_pillContainer, winrt::box_value(winrt::hstring(info)));
    if (g_flyoutTrackItem) {
        g_flyoutTrackItem.Text(winrt::hstring(info));
    }

    // Smooth interpolated progress at 20 FPS on UI thread
    if (snap.hasSession && snap.durationSec > 0.5) {
        double pos = snap.positionSec;
        if (snap.isPlaying) {
            double dtSinceSnap =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - snap.capturedAt)
                    .count();
            if (dtSinceSnap > 0.0 && dtSinceSnap < 2.0) {
                pos += dtSinceSnap;
            }
        }
        SetProgressArc(pos / snap.durationSec);
    } else {
        SetProgressArc(0.0);
    }
}

static void RemovePillFromTaskbar();

static wuxc::Grid MakeSkipButton(
    std::initializer_list<std::initializer_list<wf::Point>> polys,
    int actionId,
    wux::Thickness margin) {
    wuxc::Grid btn;
    btn.Width(20.0);
    btn.Height(20.0);
    btn.Margin(margin);
    btn.Background(wuxm::SolidColorBrush({1, 0, 0, 0}));

    wuxs::Ellipse bg;
    bg.Width(20.0);
    bg.Height(20.0);
    bg.Fill(wuxm::SolidColorBrush({85, 0, 0, 0}));
    bg.IsHitTestVisible(false);
    btn.Children().Append(bg);

    wuxs::Path icon;
    icon.Data(MakePolyGeometry(polys));
    icon.Fill(wuxm::SolidColorBrush({255, 255, 255, 255}));
    icon.Width(6.5);
    icon.Height(7.0);
    icon.HorizontalAlignment(wux::HorizontalAlignment::Center);
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    icon.IsHitTestVisible(false);
    btn.Children().Append(icon);

    btn.PointerEntered([bg](auto const&, auto const&) {
        bg.Fill(wuxm::SolidColorBrush({130, 0, 0, 0}));
    });
    btn.PointerExited([bg](auto const&, auto const&) {
        bg.Fill(wuxm::SolidColorBrush({85, 0, 0, 0}));
    });
    btn.PointerPressed([actionId](auto const&, wuxi::PointerRoutedEventArgs const& e) {
        if (e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) {
            e.Handled(true);
            g_pendingAction.store(actionId);
            if (g_mediaActionEvent) SetEvent(g_mediaActionEvent);
        }
    });
    return btn;
}

static wuxc::Grid BuildCapsuleWidget() {
    wuxc::Grid root;
    root.Name(L"MediaPillWidget_Root");
    root.Width(106.0);
    root.Height(34.0);
    root.VerticalAlignment(wux::VerticalAlignment::Center);
    root.Margin({4.0, 0.0, 6.0, 0.0});

    // 1. Base dark frosted capsule
    wuxc::Border baseBg;
    baseBg.CornerRadius({17.0, 17.0, 17.0, 17.0});
    baseBg.Background(wuxm::SolidColorBrush({255, 42, 42, 46}));
    root.Children().Append(baseBg);

    // 2. Album art capsule using Canvas + exact pixel centering + DirectComposition rounded clip
    wuxc::Canvas artHost;
    artHost.Width(106.0);
    artHost.Height(34.0);
    artHost.IsHitTestVisible(false);
    try {
        auto visual = wuxh::ElementCompositionPreview::GetElementVisual(artHost);
        auto comp = visual.Compositor();
        auto rounded = comp.CreateRoundedRectangleGeometry();
        rounded.Size({106.0f, 34.0f});
        rounded.CornerRadius({17.0f, 17.0f});
        visual.Clip(comp.CreateGeometricClip(rounded));
    } catch (...) {}

    g_artHost = artHost;
    root.Children().Append(artHost);

    // 4. Glassy highlight border (no dark gradient over album art)
    wuxc::Border overlay;
    overlay.CornerRadius({17.0, 17.0, 17.0, 17.0});
    overlay.BorderThickness({1.0, 1.0, 1.0, 1.0});
    overlay.BorderBrush(wuxm::SolidColorBrush({55, 255, 255, 255}));
    overlay.IsHitTestVisible(false);
    root.Children().Append(overlay);

    // 4. Centered controls: Prev + Play/Pause + Next
    wuxc::StackPanel controls;
    controls.Orientation(wuxc::Orientation::Horizontal);
    controls.HorizontalAlignment(wux::HorizontalAlignment::Center);
    controls.VerticalAlignment(wux::VerticalAlignment::Center);
    g_controlsPanel = controls;

    // --- Previous button (20x20) ---
    controls.Children().Append(MakeSkipButton(
        {
            {{0.0f, 0.0f}, {1.5f, 0.0f}, {1.5f, 7.0f}, {0.0f, 7.0f}},
            {{6.5f, 0.0f}, {1.5f, 3.5f}, {6.5f, 7.0f}},
        },
        3,
        {0.0, 0.0, 7.0, 0.0}));

    // --- Play / Pause button (24x24) ---
    wuxc::Grid playBtn;
    playBtn.Width(24.0);
    playBtn.Height(24.0);
    playBtn.Margin({0.0, 0.0, 7.0, 0.0});
    playBtn.Background(wuxm::SolidColorBrush({1, 0, 0, 0}));  // hit-testable

    wuxs::Ellipse playBg;
    playBg.Width(24.0);
    playBg.Height(24.0);
    playBg.Fill(wuxm::SolidColorBrush({85, 0, 0, 0}));
    playBg.IsHitTestVisible(false);
    playBtn.Children().Append(playBg);

    wuxs::Ellipse trackRing;
    trackRing.Width(22.0);
    trackRing.Height(22.0);
    trackRing.Stroke(wuxm::SolidColorBrush({50, 255, 255, 255}));
    trackRing.StrokeThickness(2.0);
    trackRing.IsHitTestVisible(false);
    playBtn.Children().Append(trackRing);

    wuxm::ArcSegment arcSeg;
    arcSeg.Point({12.0, 1.0});
    arcSeg.Size({11.0, 11.0});
    arcSeg.SweepDirection(wuxm::SweepDirection::Clockwise);
    arcSeg.IsLargeArc(false);
    g_arcSeg = arcSeg;

    wuxm::PathFigure fig;
    fig.StartPoint({12.0, 1.0});
    fig.IsClosed(false);
    fig.Segments().Append(arcSeg);

    wuxm::PathGeometry geo;
    geo.Figures().Append(fig);

    wuxs::Path arcPath;
    arcPath.Width(24.0);
    arcPath.Height(24.0);
    arcPath.Stroke(wuxm::SolidColorBrush({255, 255, 255, 255}));
    arcPath.StrokeThickness(2.0);
    arcPath.StrokeStartLineCap(wuxm::PenLineCap::Round);
    arcPath.StrokeEndLineCap(wuxm::PenLineCap::Round);
    arcPath.Opacity(0.0);
    arcPath.Data(geo);
    arcPath.IsHitTestVisible(false);
    g_progressArc = arcPath;
    playBtn.Children().Append(arcPath);

    wuxs::Path iconPlay;
    iconPlay.Data(MakePolyGeometry({{{0.0f, 0.0f}, {7.0f, 4.5f}, {0.0f, 9.0f}}}));
    iconPlay.Fill(wuxm::SolidColorBrush({255, 255, 255, 255}));
    iconPlay.Width(7.0);
    iconPlay.Height(9.0);
    iconPlay.HorizontalAlignment(wux::HorizontalAlignment::Center);
    iconPlay.VerticalAlignment(wux::VerticalAlignment::Center);
    iconPlay.Margin({1.5, 0.0, 0.0, 0.0});
    iconPlay.IsHitTestVisible(false);
    g_iconPlay = iconPlay;
    playBtn.Children().Append(iconPlay);

    wuxs::Path iconPause;
    iconPause.Data(MakePolyGeometry({
        {{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 8.0f}, {0.0f, 8.0f}},
        {{4.5f, 0.0f}, {6.5f, 0.0f}, {6.5f, 8.0f}, {4.5f, 8.0f}},
    }));
    iconPause.Fill(wuxm::SolidColorBrush({255, 255, 255, 255}));
    iconPause.Width(6.5);
    iconPause.Height(8.0);
    iconPause.HorizontalAlignment(wux::HorizontalAlignment::Center);
    iconPause.VerticalAlignment(wux::VerticalAlignment::Center);
    iconPause.Visibility(wux::Visibility::Collapsed);
    iconPause.IsHitTestVisible(false);
    g_iconPause = iconPause;
    playBtn.Children().Append(iconPause);

    playBtn.PointerEntered([playBg](auto const&, auto const&) {
        playBg.Fill(wuxm::SolidColorBrush({130, 0, 0, 0}));
    });
    playBtn.PointerExited([playBg](auto const&, auto const&) {
        playBg.Fill(wuxm::SolidColorBrush({85, 0, 0, 0}));
    });
    playBtn.PointerPressed([](auto const&, wuxi::PointerRoutedEventArgs const& e) {
        if (e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) {
            e.Handled(true);
            g_pendingAction.store(1);
            if (g_mediaActionEvent) SetEvent(g_mediaActionEvent);
        }
    });
    controls.Children().Append(playBtn);

    // --- Next button (20x20) ---
    controls.Children().Append(MakeSkipButton(
        {
            {{0.0f, 0.0f}, {5.0f, 3.5f}, {0.0f, 7.0f}},
            {{5.0f, 0.0f}, {6.5f, 0.0f}, {6.5f, 7.0f}, {5.0f, 7.0f}},
        },
        2,
        {0.0, 0.0, 0.0, 0.0}));

    root.Children().Append(controls);
    UpdateControlsFadeOnUi(false);

    root.PointerEntered([](auto const&, auto const&) {
        g_pillHovered = true;
        UpdateControlsFadeOnUi(true);
    });
    root.PointerExited([](auto const&, auto const&) {
        g_pillHovered = false;
        UpdateControlsFadeOnUi(true);
    });

    // Double-click capsule background opens active player
    root.DoubleTapped([](auto const&, auto const&) {
        std::wstring appId;
        {
            std::lock_guard<std::mutex> lk(g_mediaMtx);
            appId = g_mediaState.appId;
        }
        FocusPlayerByAppId(appId);
    });

    // Context flyout on right-click
    wuxc::MenuFlyout flyout;
    wuxc::MenuFlyoutItem trackItem;
    trackItem.Text(L"Sin reproducción");
    trackItem.IsEnabled(false);
    g_flyoutTrackItem = trackItem;
    flyout.Items().Append(trackItem);

    wuxc::MenuFlyoutSeparator sep;
    flyout.Items().Append(sep);

    wuxc::MenuFlyoutItem openItem;
    openItem.Text(L"Abrir reproductor");
    openItem.Click([](auto const&, auto const&) {
        std::wstring appId;
        {
            std::lock_guard<std::mutex> lk(g_mediaMtx);
            appId = g_mediaState.appId;
        }
        FocusPlayerByAppId(appId);
    });
    flyout.Items().Append(openItem);

    root.RightTapped([flyout](wf::IInspectable const& sender, wuxi::RightTappedRoutedEventArgs const& e) {
        e.Handled(true);
        if (auto fe = sender.try_as<wux::FrameworkElement>()) {
            flyout.ShowAt(fe);
        }
    });

    root.Unloaded([weakRoot = winrt::make_weak(root)](auto const&, auto const&) {
        if (g_unloading) return;
        auto currentRoot = weakRoot.get();
        if (currentRoot && g_pillContainer == currentRoot) {
            RemovePillFromTaskbar();
        }
    });

    return root;
}

static void RemovePillFromTaskbar() {
    if (g_uiTimer) {
        try {
            g_uiTimer.Stop();
            if (g_uiTimerToken.value) {
                g_uiTimer.Tick(g_uiTimerToken);
                g_uiTimerToken = {};
            }
        } catch (...) {}
        g_uiTimer = nullptr;
    }

    if (g_injectedParent && g_pillContainer) {
        try {
            uint32_t idx = 0;
            if (g_injectedParent.Children().IndexOf(g_pillContainer, idx)) {
                g_injectedParent.Children().RemoveAt(idx);
            }
            if (g_injectedColumnCreated) {
                if (auto grid = g_injectedParent.try_as<wuxc::Grid>()) {
                    for (auto const& ch : grid.Children()) {
                        if (auto fe = ch.try_as<wux::FrameworkElement>()) {
                            int col = wuxc::Grid::GetColumn(fe);
                            if (col > 0) wuxc::Grid::SetColumn(fe, col - 1);
                        }
                    }
                    if (grid.ColumnDefinitions().Size() > 0) {
                        grid.ColumnDefinitions().RemoveAt(0);
                    }
                }
                g_injectedColumnCreated = false;
            }
        } catch (...) {}
    }

    g_injectedParent = nullptr;
    g_pillContainer = nullptr;
    g_artHost = nullptr;
    g_controlsPanel = nullptr;
    g_progressArc = nullptr;
    g_arcSeg = nullptr;
    g_iconPlay = nullptr;
    g_iconPause = nullptr;
    g_flyoutTrackItem = nullptr;
    g_pillHovered = false;
    g_uiHasArt = false;
    g_uiIsPlaying = false;
    g_controlsVisible = true;
}

static void InjectPillIntoSystemTrayGrid(wuxc::Panel const& trayPanel) {
    if (!trayPanel || g_pillContainer || g_unloading) return;

    // Avoid double injection if an earlier instance somehow remained
    for (auto const& ch : trayPanel.Children()) {
        if (auto fe = ch.try_as<wux::FrameworkElement>()) {
            if (fe.Name() == L"MediaPillWidget_Root") return;
        }
    }

    try {
        g_pillContainer = BuildCapsuleWidget();
        g_injectedParent = trayPanel;
        g_injectedColumnCreated = false;

        if (auto stack = trayPanel.try_as<wuxc::StackPanel>()) {
            stack.Children().InsertAt(0, g_pillContainer);
        } else if (auto grid = trayPanel.try_as<wuxc::Grid>()) {
            for (auto const& ch : grid.Children()) {
                if (auto fe = ch.try_as<wux::FrameworkElement>()) {
                    int col = wuxc::Grid::GetColumn(fe);
                    wuxc::Grid::SetColumn(fe, col + 1);
                }
            }
            wuxc::ColumnDefinition colDef;
            colDef.Width(wux::GridLengthHelper::Auto());
            grid.ColumnDefinitions().InsertAt(0, colDef);
            wuxc::Grid::SetColumn(g_pillContainer, 0);
            grid.Children().InsertAt(0, g_pillContainer);
            g_injectedColumnCreated = true;
        } else {
            g_pillContainer = nullptr;
            g_injectedParent = nullptr;
            return;
        }

        // Start 50ms UI refresh timer directly on the taskbar XAML UI thread
        g_uiThumbVersion = 0;
        g_uiTimer = wux::DispatcherTimer();
        g_uiTimer.Interval(std::chrono::milliseconds(50));
        g_uiTimerToken = g_uiTimer.Tick([](auto const&, auto const&) {
            RefreshUiFromMediaSnapshot();
        });
        g_uiTimer.Start();
        RefreshUiFromMediaSnapshot();

        Wh_Log(L"Media Pill injected natively into SystemTrayFrameGrid!");
        DebugLog(L"InjectPillIntoSystemTrayGrid: SUCCESS");
    } catch (...) {
        WCHAR buf[128];
        wsprintfW(buf, L"InjectPillIntoSystemTrayGrid: FAILED hr=0x%08X", winrt::to_hresult());
        DebugLog(buf);
        Wh_Log(L"Failed to inject Media Pill: %08X", winrt::to_hresult());
        RemovePillFromTaskbar();
    }
}

// ---------------------------------------------------------------------------
// VisualTreeWatcher implementation
// ---------------------------------------------------------------------------
VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
    : m_xamlDiagnostics(site.as<IXamlDiagnostics>()) {
    m_xamlDiagnostics->QueryInterface(IID_IXamlDiagnosticsTestHooks, m_testHooks.put_void());
    DebugLog(L"VisualTreeWatcher constructed, starting Advise thread");

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID lpParam) -> DWORD {
            auto* watcher = reinterpret_cast<VisualTreeWatcher*>(lpParam);
            auto service = watcher->m_xamlDiagnostics.as<IVisualTreeService3>();
            g_reportCompositionDiagAsDisabled = true;
            HRESULT hr = service->AdviseVisualTreeChange(watcher);
            g_reportCompositionDiagAsDisabled = false;
            WCHAR buf[128];
            wsprintfW(buf, L"AdviseVisualTreeChange hr=0x%08X", hr);
            DebugLog(buf);
            watcher->Release();
            if (FAILED(hr)) {
                Wh_Log(L"AdviseVisualTreeChange failed: %08X", hr);
            }
            return 0;
        },
        this, 0, nullptr);
    if (thread) {
        AddRef();
        CloseHandle(thread);
    }
}

void VisualTreeWatcher::UnadviseVisualTreeChange() {
    try {
        m_xamlDiagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(this);
    } catch (...) {}
}

std::vector<DWORD> VisualTreeWatcher::GetCallbackThreadIds() {
    winrt::slim_lock_guard lk(m_threadLock);
    return m_callbackThreadIds;
}

void VisualTreeWatcher::RecordCallbackThread() {
    DWORD tid = GetCurrentThreadId();
    winrt::slim_lock_guard lk(m_threadLock);
    if (std::find(m_callbackThreadIds.begin(), m_callbackThreadIds.end(), tid) ==
        m_callbackThreadIds.end()) {
        m_callbackThreadIds.push_back(tid);
    }
}

void VisualTreeWatcher::ReleaseHandle(InstanceHandle handle) {
    if (m_testHooks && handle) {
        m_testHooks->UnregisterInstance(handle);
    }
}

wf::IInspectable VisualTreeWatcher::FromHandle(InstanceHandle handle) {
    wf::IInspectable obj;
    winrt::check_hresult(m_xamlDiagnostics->GetIInspectableFromHandle(
        handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
    return obj;
}

static std::atomic<int> g_vtcLogCount{0};

HRESULT VisualTreeWatcher::OnVisualTreeChange(
    ParentChildRelation relation,
    VisualElement element,
    VisualMutationType mutationType) try {
    RecordCallbackThread();

    if (mutationType == Add) {
        int cnt = g_vtcLogCount.fetch_add(1);
        if (cnt < 8 ||
            (element.Name && (wcsstr(element.Name, L"Tray") || wcsstr(element.Name, L"RootGrid") || wcsstr(element.Name, L"Taskbar"))) ||
            (element.Type && (wcsstr(element.Type, L"SystemTray") || wcsstr(element.Type, L"Taskbar")))) {
            WCHAR buf[300];
            _snwprintf_s(buf, _TRUNCATE, L"VTC[%d] type='%s' name='%s' uiTid=%lu",
                         cnt,
                         element.Type ? element.Type : L"(null)",
                         element.Name ? element.Name : L"(null)",
                         g_taskbarUiThreadId);
            DebugLog(buf);
        }

        if (cnt < 12) {
            ::IInspectable* rawTest = nullptr;
            HRESULT hrTest = m_xamlDiagnostics->GetIInspectableFromHandle(element.Handle, &rawTest);
            if (rawTest) rawTest->Release();
            WCHAR hbuf[256];
            _snwprintf_s(hbuf, _TRUNCATE, L"VTC[%d] handle=0x%llX parent=0x%llX GetInspectableHr=0x%08X",
                         cnt, (unsigned long long)element.Handle,
                         (unsigned long long)relation.Parent, hrTest);
            DebugLog(hbuf);
        }

        if (element.Name && wcscmp(element.Name, L"SystemTrayFrameGrid") == 0) {
            DebugLog(L"VTC: Entering SystemTrayFrameGrid branch");
            if (g_taskbarUiThreadId == 0 || GetCurrentThreadId() == g_taskbarUiThreadId) {
                auto obj = FromHandle(element.Handle);
                DebugLog(obj ? L"VTC: FromHandle returned non-null" : L"VTC: FromHandle returned NULL");
                if (auto fe = obj.try_as<wux::FrameworkElement>()) {
                    DebugLog(L"OnVisualTreeChange: Matched SystemTrayFrameGrid!");
                    if (auto panel = fe.try_as<wuxc::Panel>()) {
                        InjectPillIntoSystemTrayGrid(panel);
                    } else {
                        DebugLog(L"OnVisualTreeChange: SystemTrayFrameGrid is NOT wuxc::Panel!");
                    }
                } else {
                    DebugLog(L"VTC: obj.try_as<FrameworkElement>() failed");
                }
            } else {
                DebugLog(L"VTC: Thread mismatch!");
            }
        }
    }

    // Do NOT unregister synchronously during Add replay!
    return S_OK;
} catch (...) {
    WCHAR ebuf[128];
    wsprintfW(ebuf, L"OnVisualTreeChange EXCEPTION hr=0x%08X", winrt::to_hresult());
    DebugLog(ebuf);
    return S_OK;
}

HRESULT VisualTreeWatcher::OnElementStateChanged(
    InstanceHandle, VisualElementState, LPCWSTR) noexcept {
    return S_OK;
}

// ---------------------------------------------------------------------------
// COM TAP ClassFactory & XAML Diagnostics hook
// ---------------------------------------------------------------------------
class MediaPillTAP
    : public winrt::implements<MediaPillTAP, IObjectWithSite, winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override try {
        DebugLog(pUnkSite ? L"MediaPillTAP::SetSite(non-null)" : L"MediaPillTAP::SetSite(null)");
        if (auto prev = ExchangeWatcher(nullptr)) {
            prev->UnadviseVisualTreeChange();
        }
        m_site.copy_from(pUnkSite);
        if (m_site) {
            FreeLibrary(GetCurrentModuleHandle());
            ExchangeWatcher(winrt::make_self<VisualTreeWatcher>(m_site));
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override {
        return m_site.as(riid, ppvSite);
    }

   private:
    winrt::com_ptr<IUnknown> m_site;
};

template <class T>
struct SimpleFactory
    : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(
        IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override try {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        *ppvObject = nullptr;
        return winrt::make<T>().as(riid, ppvObject);
    } catch (...) {
        return winrt::to_hresult();
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport)
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    if (rclsid == CLSID_WindhawkMediaPillTAP) {
        *ppv = nullptr;
        return winrt::make<SimpleFactory<MediaPillTAP>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return winrt::to_hresult();
}

__declspec(dllexport)
STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

static HRESULT InjectWindhawkTAP() noexcept {
    HMODULE mod = GetCurrentModuleHandle();
    if (!mod) return HRESULT_FROM_WIN32(GetLastError());

    WCHAR location[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(mod, location, ARRAYSIZE(location));
    if (len == 0 || len >= ARRAYSIZE(location)) return HRESULT_FROM_WIN32(GetLastError());

    HMODULE wux = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wux) return HRESULT_FROM_WIN32(GetLastError());

    using PFN_IXDE = decltype(&InitializeXamlDiagnosticsEx);
    auto ixde = reinterpret_cast<PFN_IXDE>(GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
    if (!ixde) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = E_FAIL;
    for (int i = 0; i < 10000; i++) {
        WCHAR conn[64];
        wsprintfW(conn, L"VisualDiagConnection%d", i + 1);
        hr = ixde(conn, GetCurrentProcessId(), L"", location, CLSID_WindhawkMediaPillTAP, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Registry hooks (suppress XAML diagnostics debug borders) & window creation hook
// ---------------------------------------------------------------------------
using RegOpenKeyExW_t = decltype(&RegOpenKeyExW);
static RegOpenKeyExW_t RegOpenKeyExW_Original = nullptr;
static LSTATUS WINAPI RegOpenKeyExW_Hook(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
                                         REGSAM samDesired, PHKEY phkResult) {
    LSTATUS res = RegOpenKeyExW_Original(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    if (res == ERROR_SUCCESS || !g_reportCompositionDiagAsDisabled ||
        hKey != HKEY_LOCAL_MACHINE || !lpSubKey ||
        _wcsicmp(lpSubKey, L"Software\\Microsoft\\XAML\\Debug") != 0) {
        return res;
    }
    return RegOpenKeyExW_Original(HKEY_LOCAL_MACHINE, L"Software\\Microsoft",
                                  ulOptions, samDesired, phkResult);
}

using RegQueryValueExW_t = decltype(&RegQueryValueExW);
static RegQueryValueExW_t RegQueryValueExW_Original = nullptr;
static LSTATUS WINAPI RegQueryValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
                                            LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {
    if (!g_reportCompositionDiagAsDisabled || !lpValueName ||
        _wcsicmp(lpValueName, L"DisableCompositionDiag") != 0) {
        return RegQueryValueExW_Original(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }
    if (lpType) *lpType = REG_DWORD;
    if (lpData && (!lpcbData || *lpcbData < sizeof(DWORD))) {
        if (lpcbData) *lpcbData = sizeof(DWORD);
        return ERROR_MORE_DATA;
    }
    if (lpData) *reinterpret_cast<DWORD*>(lpData) = 1;
    if (lpcbData) *lpcbData = sizeof(DWORD);
    return ERROR_SUCCESS;
}

static void InitializeOnUiThread() {
    g_initializedForThread = true;
    g_taskbarUiThreadId = GetCurrentThreadId();
}

static void StartTapIfReady() {
    if (!g_initialized.exchange(true)) {
        HRESULT hr = InjectWindhawkTAP();
        WCHAR buf[128];
        wsprintfW(buf, L"InjectWindhawkTAP: hr=0x%08X", hr);
        DebugLog(buf);
        if (FAILED(hr)) {
            Wh_Log(L"InjectWindhawkTAP error: %08X", hr);
        }
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
static CreateWindowExW_t CreateWindowExW_Original = nullptr;
static HWND WINAPI CreateWindowExW_Hook(
    DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, PVOID lpParam) {
    HWND hWnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                         X, Y, nWidth, nHeight, hWndParent, hMenu,
                                         hInstance, lpParam);
    if (hWnd && hWndParent) {
        WCHAR cls[64]{};
        WCHAR pcls[64]{};
        if (GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) &&
            _wcsicmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 &&
            GetClassNameW(hWndParent, pcls, ARRAYSIZE(pcls)) &&
            _wcsicmp(pcls, L"Shell_TrayWnd") == 0) {
            InitializeOnUiThread();
            StartTapIfReady();
        }
    }
    return hWnd;
}

// ---------------------------------------------------------------------------
// Windhawk entrypoints
// ---------------------------------------------------------------------------
static void LoadSettings() {
    g_settings.hideWhenIdle = Wh_GetIntSetting(L"hideWhenIdle") != 0;
}

BOOL Wh_ModInit() {
    DebugLog(L"Wh_ModInit", true);
    g_unloading = false;
    LoadSettings();

    WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                   &CreateWindowExW_Original);

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelBase) {
        auto pRegOpen = reinterpret_cast<RegOpenKeyExW_t>(GetProcAddress(kernelBase, "RegOpenKeyExW"));
        auto pRegQuery = reinterpret_cast<RegQueryValueExW_t>(GetProcAddress(kernelBase, "RegQueryValueExW"));
        if (pRegOpen) WindhawkUtils::SetFunctionHook(pRegOpen, RegOpenKeyExW_Hook, &RegOpenKeyExW_Original);
        if (pRegQuery) WindhawkUtils::SetFunctionHook(pRegQuery, RegQueryValueExW_Hook, &RegQueryValueExW_Original);
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    g_mediaStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_mediaActionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mediaThread = std::thread(MediaWorkerProc);

    HWND hTaskbarUiWnd = GetTaskbarUiWnd();
    WCHAR buf[128];
    wsprintfW(buf, L"Wh_ModAfterInit: hTaskbarUiWnd=0x%p", hTaskbarUiWnd);
    DebugLog(buf);
    if (hTaskbarUiWnd) {
        RunFromWindowThread(
            hTaskbarUiWnd, [](PVOID) { InitializeOnUiThread(); }, nullptr);
        StartTapIfReady();
    }
}

void Wh_ModUninit() {
    g_unloading = true;
    if (g_mediaStopEvent) SetEvent(g_mediaStopEvent);
    if (g_mediaThread.joinable()) g_mediaThread.join();
    if (g_mediaStopEvent) { CloseHandle(g_mediaStopEvent); g_mediaStopEvent = nullptr; }
    if (g_mediaActionEvent) { CloseHandle(g_mediaActionEvent); g_mediaActionEvent = nullptr; }

    auto watcher = ExchangeWatcher(nullptr);
    if (watcher) watcher->UnadviseVisualTreeChange();

    HWND hTaskbarUiWnd = GetTaskbarUiWnd();
    if (hTaskbarUiWnd) {
        RunFromWindowThread(
            hTaskbarUiWnd,
            [](PVOID) {
                RemovePillFromTaskbar();
                g_initializedForThread = false;
            },
            nullptr);
    }

    if (watcher) {
        for (DWORD tid : watcher->GetCallbackThreadIds()) {
            HWND h = FindThreadWindow(tid);
            DWORD pid = 0;
            if (h && GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId()) {
                SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, nullptr);
            }
        }
    }
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    HWND hTaskbarUiWnd = GetTaskbarUiWnd();
    if (hTaskbarUiWnd) {
        RunFromWindowThread(
            hTaskbarUiWnd, [](PVOID) { RefreshUiFromMediaSnapshot(); }, nullptr);
    }
}
