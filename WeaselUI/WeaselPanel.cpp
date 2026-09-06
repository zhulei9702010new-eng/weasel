#include "stdafx.h"
#include "WeaselPanel.h"

#include <utility>
#include <ShellScalingApi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <appmodel.h>
#include <string>
#include <atomic>
#include <new>
#include <VersionHelpers.hpp>
#include <WeaselIPCData.h>
#include <algorithm>

#include "VerticalLayout.h"
#include "HorizontalLayout.h"
#include "FullScreenLayout.h"
#include "VHorizontalLayout.h"

// for IDI_ZH, IDI_EN
#include <resource.h>
#define COLORTRANSPARENT(color) ((color & 0xff000000) == 0)
#define COLORNOTTRANSPARENT(color) ((color & 0xff000000) != 0)
#define TRANS_COLOR 0x00000000
#define GDPCOLOR_FROM_COLORREF(color)                                \
  Gdiplus::Color::MakeARGB(((color >> 24) & 0xff), GetRValue(color), \
                           GetGValue(color), GetBValue(color))
#define HALF_ALPHA_COLOR(color) \
  ((((color & 0xff000000) >> 25) & 0xff) << 24) | (color & 0x00ffffff)

#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace {

// Numeric values keep the PoC buildable with Weasel's current
// Windows SDK while DWM evaluates the attributes at runtime.
constexpr DWORD kDwmaUseHostBackdropBrush = 17;
constexpr DWORD kDwmaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmaWindowCornerPreference = 33;
constexpr DWORD kDwmaBorderColor = 34;

constexpr int kDwmwcpRound = 2;  // DWMWCP_ROUND
constexpr COLORREF kDwmColorNone = 0xFFFFFFFEu;
constexpr BYTE kAcrylicTintAlpha = 0x18;

constexpr wchar_t kWeaselAcrylicBackdropClass[] = L"WeaselAcrylicBackdropHost";

// CI #22: follow the real candidate HWND, including position changes made by
// the host application rather than MoveTo(). Install only for local Acrylic;
// the CI #21 Settings protocol and its hidden client host are unchanged.
constexpr wchar_t kLocalAcrylicGeometryProperty[] =
    L"WeaselAcrylicLocalGeometryState";
constexpr wchar_t kLocalAcrylicGeometryPolicy[] =
    L"WeaselAcrylicGeometryPolicy";
constexpr UINT_PTR kLocalAcrylicGeometrySubclass = 0x57414731;

struct LocalAcrylicGeometryState {
  HWND candidate = nullptr;
  WeaselPanel* panel = nullptr;
  unsigned references = 1;  // Owned by the subclass, on this UI thread only.
  unsigned depth = 0;
  bool installed = false;
  bool pending = false;
  bool syncing = false;
};

LocalAcrylicGeometryState* LocalAcrylicGeometry(HWND hwnd) {
  return hwnd ? reinterpret_cast<LocalAcrylicGeometryState*>(
                    ::GetPropW(hwnd, kLocalAcrylicGeometryProperty))
              : nullptr;
}

void ReleaseLocalAcrylicGeometry(LocalAcrylicGeometryState* state) {
  if (state && --state->references == 0)
    delete state;
}

class LocalAcrylicGeometryRef {
 public:
  explicit LocalAcrylicGeometryRef(LocalAcrylicGeometryState* state)
      : state_(state) {
    if (state_)
      ++state_->references;
  }
  ~LocalAcrylicGeometryRef() { ReleaseLocalAcrylicGeometry(state_); }
  LocalAcrylicGeometryRef(const LocalAcrylicGeometryRef&) = delete;
  LocalAcrylicGeometryRef& operator=(const LocalAcrylicGeometryRef&) = delete;

 private:
  LocalAcrylicGeometryState* state_;
};

void RequestLocalAcrylicGeometry(LocalAcrylicGeometryState* state) {
  if (!state || !state->panel)
    return;
  LocalAcrylicGeometryRef hold(state);
  state->pending = true;
  if (state->depth || state->syncing)
    return;

  // Updating the second HWND can itself produce window messages. Coalesce a
  // real follow-up, but never recurse or run an unbounded positioning loop.
  for (unsigned pass = 0; pass < 2 && state->pending && state->panel; ++pass) {
    state->pending = false;
    state->syncing = true;
    state->panel->ShowAcrylicBackdrop();
    state->syncing = false;
  }
}

bool DeferLocalAcrylicGeometry(HWND hwnd) {
  auto state = LocalAcrylicGeometry(hwnd);
  if (!state || !state->depth)
    return false;
  state->pending = true;
  return true;
}

class LocalAcrylicGeometryBatch {
 public:
  explicit LocalAcrylicGeometryBatch(HWND hwnd)
      : state_(LocalAcrylicGeometry(hwnd)), hold_(state_) {
    if (state_) {
      ++state_->depth;
      // Layout can change its content inset without a HWND size change.
      state_->pending = true;
    }
  }
  ~LocalAcrylicGeometryBatch() {
    if (state_ && --state_->depth == 0 && state_->pending)
      RequestLocalAcrylicGeometry(state_);
  }
  LocalAcrylicGeometryBatch(const LocalAcrylicGeometryBatch&) = delete;
  LocalAcrylicGeometryBatch& operator=(const LocalAcrylicGeometryBatch&) =
      delete;

 private:
  LocalAcrylicGeometryState* state_;
  LocalAcrylicGeometryRef hold_;
};

LRESULT CALLBACK LocalAcrylicGeometryProc(HWND hwnd,
                                          UINT message,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          UINT_PTR id,
                                          DWORD_PTR data);

void DetachLocalAcrylicGeometry(LocalAcrylicGeometryState* state,
                                bool destroying = false) {
  if (!state)
    return;
  state->panel = nullptr;
  state->pending = false;
  if (LocalAcrylicGeometry(state->candidate) == state) {
    ::RemovePropW(state->candidate, kLocalAcrylicGeometryProperty);
    ::RemovePropW(state->candidate, kLocalAcrylicGeometryPolicy);
  }
  if (state->installed) {
    const BOOL removed =
        ::RemoveWindowSubclass(state->candidate, LocalAcrylicGeometryProc,
                               kLocalAcrylicGeometrySubclass);
    // If removal fails, keep the callback's reference until WM_NCDESTROY.
    // A callback left installed must never point at freed state or panel data.
    if (removed || destroying) {
      state->installed = false;
      ReleaseLocalAcrylicGeometry(state);
    }
  }
}

void RemoveLocalAcrylicGeometry(HWND hwnd) {
  DetachLocalAcrylicGeometry(LocalAcrylicGeometry(hwnd));
}

LRESULT CALLBACK LocalAcrylicGeometryProc(HWND hwnd,
                                          UINT message,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          UINT_PTR id,
                                          DWORD_PTR data) {
  auto state = reinterpret_cast<LocalAcrylicGeometryState*>(data);
  LocalAcrylicGeometryRef hold(state);
  if (message == WM_NCDESTROY) {
    DetachLocalAcrylicGeometry(state, true);
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
  }

  // Let ATL and the application's existing handlers finish first. No layout
  // or pointer is read until the final HWND state is available. Destruction
  // during default processing nulls panel while hold keeps this state alive.
  const LRESULT result = ::DefSubclassProc(hwnd, message, wParam, lParam);
  if (message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW)
    RequestLocalAcrylicGeometry(state);
  return result;
}

bool InstallLocalAcrylicGeometry(HWND hwnd, WeaselPanel* panel) {
  if (!hwnd || !panel)
    return false;
  if (LocalAcrylicGeometry(hwnd))
    return true;
  auto state = new (std::nothrow) LocalAcrylicGeometryState;
  if (!state)
    return false;
  state->candidate = hwnd;
  state->panel = panel;
  if (!::SetPropW(hwnd, kLocalAcrylicGeometryProperty,
                  reinterpret_cast<HANDLE>(state))) {
    ReleaseLocalAcrylicGeometry(state);
    return false;
  }
  if (!::SetWindowSubclass(hwnd, LocalAcrylicGeometryProc,
                           kLocalAcrylicGeometrySubclass,
                           reinterpret_cast<DWORD_PTR>(state))) {
    ::RemovePropW(hwnd, kLocalAcrylicGeometryProperty);
    ReleaseLocalAcrylicGeometry(state);
    return false;
  }
  state->installed = true;
  ::SetPropW(hwnd, kLocalAcrylicGeometryPolicy,
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)));
  return true;
}

bool LocalAcrylicGeometryMatches(HWND backdrop,
                                 HWND candidate,
                                 int x,
                                 int y,
                                 int width,
                                 int height) {
  RECT actual = {};
  return ::IsWindowVisible(backdrop) && ::GetWindowRect(backdrop, &actual) &&
         actual.left == x && actual.top == y && actual.right == x + width &&
         actual.bottom == y + height &&
         ::GetWindow(candidate, GW_HWNDNEXT) == backdrop;
}

COLORREF WithAlpha(COLORREF color, BYTE alpha) {
  return (color & 0x00FFFFFFu) | (static_cast<COLORREF>(alpha) << 24);
}

bool IsDarkColor(COLORREF color) {
  const int luminance =
      GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114;
  return luminance < 128000;
}

bool HandleExternalAcrylicMessage(HWND hwnd,
                                  UINT message,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  LRESULT& result);

LRESULT CALLBACK AcrylicBackdropWndProc(HWND hwnd,
                                        UINT message,
                                        WPARAM wParam,
                                        LPARAM lParam) {
  LRESULT result = 0;
  if (HandleExternalAcrylicMessage(hwnd, message, wParam, lParam, result))
    return result;
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCACTIVATE:
      return DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, lParam);
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

HMODULE AcrylicWindowModule() {
  static int moduleAnchor = 0;
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module))
    return nullptr;
  return module;
}

bool EnsureAcrylicBackdropClass() {
  const HMODULE module = AcrylicWindowModule();
  if (!module)
    return false;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = AcrylicBackdropWndProc;
  wc.hInstance = module;
  wc.lpszClassName = kWeaselAcrylicBackdropClass;

  if (RegisterClassExW(&wc))
    return true;

  if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  WNDCLASSEXW existing{};
  existing.cbSize = sizeof(existing);
  return ::GetClassInfoExW(module, kWeaselAcrylicBackdropClass, &existing) &&
         existing.lpfnWndProc == AcrylicBackdropWndProc;
}

constexpr wchar_t kWeaselAcrylicAppSdkDll[] = L"WeaselAcrylicAppSdk.dll";
constexpr wchar_t kWeaselAcrylicAppSdkActiveProperty[] =
    L"WeaselAcrylicAppSdkActive";
constexpr wchar_t kAcrylicStageProperty[] = L"WeaselAcrylicAppSdkStage";
constexpr wchar_t kAcrylicHrProperty[] = L"WeaselAcrylicAppSdkHresult";
constexpr wchar_t kAcrylicPolicyProperty[] = L"WeaselAcrylicAppSdkPolicy";

void SetAcrylicDiagnostic(HWND hwnd, LONG stage, HRESULT hr) {
  ::SetPropW(hwnd, kAcrylicPolicyProperty,
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(3)));
  ::SetPropW(hwnd, kAcrylicStageProperty,
             reinterpret_cast<HANDLE>(
                 static_cast<ULONG_PTR>(static_cast<DWORD>(stage))));
  ::SetPropW(
      hwnd, kAcrylicHrProperty,
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(static_cast<DWORD>(hr))));
}

// The installer/TSF language-bar code use the HKLM WeaselRoot value. In an
// in-process TSF client, GetModuleFileName(nullptr) is WINWORD/Chrome, not
// Rime.
HRESULT ReadAcrylicInstallRoot(std::wstring& root) {
  // Open each registry view explicitly. The RRF_SUBKEY_WOW64* flags are
  // not declared when WeaselUI targets Windows 8.1 (_WIN32_WINNT=0x0603).
  const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
  for (REGSAM view : views) {
    HKEY key = nullptr;
    const LSTATUS opened =
        ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel", 0,
                        KEY_QUERY_VALUE | view, &key);
    if (opened != ERROR_SUCCESS)
      continue;
    WCHAR value[32768] = {};
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    const LSTATUS result = ::RegGetValueW(
        key, nullptr, L"WeaselRoot", RRF_RT_REG_SZ, nullptr, value, &bytes);
    ::RegCloseKey(key);
    if (result != ERROR_SUCCESS)
      continue;
    root = value;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
      root.pop_back();
    // Only a local absolute installation path; no CWD/PATH, HKCU, or network
    // directory is used to locate executable plugin code in another process.
    if (root.size() >= 3 &&
        ((root[0] >= L'A' && root[0] <= L'Z') ||
         (root[0] >= L'a' && root[0] <= L'z')) &&
        root[1] == L':' && (root[2] == L'\\' || root[2] == L'/'))
      return S_OK;
    return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  }
  return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

class AcrylicAppSdkBridge {
 public:
  bool TryInitialize(HWND hwnd, BOOL darkMode, bool inServer) {
    if (!hwnd || !::IsWindow(hwnd))
      return false;
#if (!defined(_M_X64) && !defined(_M_IX86)) || defined(_M_ARM64EC)
    // Build-matched x64 and x86 helpers are packaged separately. Native ARM
    // clients keep the normal skin without loading an incompatible DLL.
    SetAcrylicDiagnostic(hwnd, -10,
                         HRESULT_FROM_WIN32(ERROR_EXE_MACHINE_TYPE_MISMATCH));
    return false;
#else
    // Protect against re-entry during LoadLibrary/bootstrap on the same UI
    // thread. The native loader cache is shared, but targets live in helper
    // TLS.
    static thread_local bool entering = false;
    if (entering) {
      SetAcrylicDiagnostic(hwnd, -11, HRESULT_FROM_WIN32(ERROR_BUSY));
      return false;
    }
    entering = true;
    struct EntryScope {
      bool& flag;
      ~EntryScope() { flag = false; }
    } scope{entering};
    LoadRequest request{this, inServer};
    if (!::InitOnceExecuteOnce(&once_, LoadCallback, &request, nullptr)) {
      SetAcrylicDiagnostic(hwnd, -20, HRESULT_FROM_WIN32(::GetLastError()));
      return false;
    }
    if (!ready_.load()) {
      SetAcrylicDiagnostic(hwnd, loadStage_, loadResult_);
      return false;
    }
    const BOOL ok = attach_(hwnd, darkMode);
    SetAcrylicDiagnostic(hwnd, lastStage_(), lastHr_());
    return ok && isWindowActive_(hwnd) && ::IsWindow(hwnd);
#endif
  }

  void SetDarkMode(HWND hwnd, BOOL darkMode) {
    if (ready_.load() && hwnd)
      setWindowTheme_(hwnd, darkMode);
  }

  void DetachWindow(HWND hwnd) {
    if (ready_.load() && hwnd)
      detach_(hwnd);
  }

  void RequestThreadShutdownIfLoaded() {
    if (ready_.load())
      requestShutdown_();
  }

 private:
  using AttachFn = BOOL(WINAPI*)(HWND, BOOL);
  using ThemeFn = void(WINAPI*)(HWND, BOOL);
  using ActiveFn = BOOL(WINAPI*)(HWND);
  using DetachFn = void(WINAPI*)(HWND);
  using RequestShutdownFn = HRESULT(WINAPI*)();
  using DiagnosticFn = LONG(WINAPI*)();
  struct LoadRequest {
    AcrylicAppSdkBridge* self;
    bool inServer;
  };

  static BOOL CALLBACK LoadCallback(PINIT_ONCE, PVOID parameter, PVOID*) {
    auto request = static_cast<LoadRequest*>(parameter);
    auto self = request->self;
    try {
      self->Load(request->inServer);
    } catch (...) {
      self->loadResult_ = E_UNEXPECTED;
    }
    // Cache failure too; do not load a runtime on every keystroke. After fixing
    // installation/runtime, restart the host app to retry with a clean graph.
    return TRUE;
  }

  void Load(bool inServer) {
    loadStage_ = -20;
    std::wstring root;
    loadResult_ = ReadAcrylicInstallRoot(root);
    if (FAILED(loadResult_) && inServer) {
      // Developer/server fallback ONLY. Never follow Word's executable path.
      WCHAR executable[32768] = {};
      const DWORD length =
          ::GetModuleFileNameW(nullptr, executable, _countof(executable));
      if (length && length < _countof(executable)) {
        root.assign(executable, length);
        const auto slash = root.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
          root.resize(slash);
          loadResult_ = S_OK;
        }
      }
    }
    if (FAILED(loadResult_))
      return;
#if defined(_M_IX86)
    // Select by THIS process's architecture, not the OS/server architecture.
    root += L"\\acrylic\\x86";
#endif
    std::wstring helperPath = root + L"\\" + kWeaselAcrylicAppSdkDll;
    loadStage_ = -30;
    module_ = ::LoadLibraryExW(
        helperPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
      const DWORD error = ::GetLastError();
      loadResult_ = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
      return;
    }
    // Keep one reference for process lifetime. Runtime objects/queues are NOT
    // unloaded when a composition ends. Policy version stops old DLL mixing.
    loadStage_ = -40;
    auto policy = reinterpret_cast<DiagnosticFn>(::GetProcAddress(
        module_, "WeaselAcrylicAppSdkGetLifetimePolicyVersion"));
    if (!policy || policy() != 3) {
      loadResult_ = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
      return;
    }
    attach_ = reinterpret_cast<AttachFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkAttach"));
    setWindowTheme_ = reinterpret_cast<ThemeFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkSetWindowTheme"));
    isWindowActive_ = reinterpret_cast<ActiveFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkIsWindowActive"));
    detach_ = reinterpret_cast<DetachFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkDetach"));
    requestShutdown_ = reinterpret_cast<RequestShutdownFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkRequestThreadShutdown"));
    lastStage_ = reinterpret_cast<DiagnosticFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkGetLastStage"));
    lastHr_ = reinterpret_cast<DiagnosticFn>(
        ::GetProcAddress(module_, "WeaselAcrylicAppSdkGetLastHresult"));
    if (!attach_ || !setWindowTheme_ || !isWindowActive_ || !detach_ ||
        !requestShutdown_ || !lastStage_ || !lastHr_) {
      loadResult_ = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
      return;
    }
    loadResult_ = S_OK;
    ready_.store(true);
  }

  INIT_ONCE once_ = INIT_ONCE_STATIC_INIT;
  std::atomic<bool> ready_{false};
  HMODULE module_ = nullptr;
  AttachFn attach_ = nullptr;
  ThemeFn setWindowTheme_ = nullptr;
  ActiveFn isWindowActive_ = nullptr;
  DetachFn detach_ = nullptr;
  RequestShutdownFn requestShutdown_ = nullptr;
  DiagnosticFn lastStage_ = nullptr;
  DiagnosticFn lastHr_ = nullptr;
  LONG loadStage_ = -20;
  HRESULT loadResult_ = E_PENDING;
};

AcrylicAppSdkBridge g_acrylicAppSdkBridge;

// Phase2 R1: optional asynchronous compatible-client -> WeaselServer lease.
// Only HWNDs and integer tokens cross processes. The client publishes the final
// content rectangle; the server never calculates a second caret/owner offset.
constexpr UINT kExternalUpdate = WM_APP + 0x51A;
constexpr UINT kExternalRelease = WM_APP + 0x51B;
constexpr UINT kExternalAck = WM_APP + 0x51C;
constexpr UINT_PTR kExternalTimer = 0x51A;
constexpr DWORD kExternalProtocol = 2;
constexpr DWORD kExternalTimeoutMs = 600;
constexpr DWORD kExternalRetryMs = 1000;
constexpr wchar_t kExtProtocol[] = L"WeaselAcrylicExternalProtocol";
constexpr wchar_t kExtServer[] = L"WeaselAcrylicExternalServer";
constexpr wchar_t kExtToken[] = L"WeaselAcrylicExternalToken";
constexpr wchar_t kExtSequence[] = L"WeaselAcrylicExternalSequence";
constexpr wchar_t kExtCandidate[] = L"WeaselAcrylicExternalCandidate";
constexpr wchar_t kExtWanted[] = L"WeaselAcrylicExternalWanted";
constexpr wchar_t kExtX[] = L"WeaselAcrylicExternalX";
constexpr wchar_t kExtY[] = L"WeaselAcrylicExternalY";
constexpr wchar_t kExtWidth[] = L"WeaselAcrylicExternalWidth";
constexpr wchar_t kExtHeight[] = L"WeaselAcrylicExternalHeight";
constexpr wchar_t kExtDark[] = L"WeaselAcrylicExternalDark";
constexpr wchar_t kExtClientPulse[] = L"WeaselAcrylicExternalClientPulse";
constexpr wchar_t kExtServerPulse[] = L"WeaselAcrylicExternalServerPulse";
constexpr wchar_t kExtOwner[] = L"WeaselAcrylicExternalOwner";
constexpr wchar_t kExtActive[] = L"WeaselAcrylicExternalActive";
constexpr wchar_t kExtAckClient[] = L"WeaselAcrylicExternalAckClient";
constexpr wchar_t kExtAckToken[] = L"WeaselAcrylicExternalAckToken";
constexpr wchar_t kExtAckSequence[] = L"WeaselAcrylicExternalAckSequence";
constexpr wchar_t kExtRenderer[] = L"WeaselAcrylicExternalRendererHwnd";
constexpr wchar_t kExtReady[] = L"WeaselAcrylicExternalReady";

struct ExternalSnapshot {
  HWND candidate = nullptr;
  DWORD token = 0;
  DWORD sequence = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  BOOL dark = FALSE;
};

struct ExternalAcrylicState {
  bool server = false;
  bool active = false;
  bool foregroundReady = false;
  bool pending = false;
  bool attempted = false;
  bool redrawing = false;
  HWND candidate = nullptr;
  HWND peer = nullptr;
  WeaselPanel* panel = nullptr;  // Local pointer only, never published via IPC.
  ExternalSnapshot snapshot;
  DWORD token = 0;
  DWORD lastAttempt = 0;
  DWORD pendingSince = 0;
  DWORD pendingSequence = 0;
  DWORD boundServerPid = 0;  // Derived locally from the connected input pipe.
  // R3 Search-only presentation guard; not a pixel-level blur detector.
  bool searchPresentation = false;
  bool presentationTiming = false;
  bool presentationRepairUsed = false;
  bool presentationBlocked = false;  // Client: retry only after hide/recreate.
  DWORD presentationSince = 0;
};

ULONG_PTR ExternalProperty(HWND hwnd, const wchar_t* name) {
  return reinterpret_cast<ULONG_PTR>(::GetPropW(hwnd, name));
}

bool SetExternalProperty(HWND hwnd, const wchar_t* name, ULONG_PTR value) {
  return ::SetPropW(hwnd, name, reinterpret_cast<HANDLE>(value)) != FALSE;
}

bool ExternalFresh(DWORD now, DWORD then) {
  return static_cast<DWORD>(now - then) <= kExternalTimeoutMs;
}

ExternalAcrylicState* ExternalState(HWND hwnd) {
  return hwnd ? reinterpret_cast<ExternalAcrylicState*>(
                    ::GetWindowLongPtrW(hwnd, GWLP_USERDATA))
              : nullptr;
}

bool IsSettingsImage(const wchar_t* path, DWORD length) {
  wchar_t windows[32768] = {};
  const UINT count = ::GetWindowsDirectoryW(windows, _countof(windows));
  if (!count || count >= _countof(windows))
    return false;
  const std::wstring expected = std::wstring(windows, count) +
                                L"\\ImmersiveControlPanel\\SystemSettings.exe";
  return ::CompareStringOrdinal(
             path, static_cast<int>(length), expected.c_str(),
             static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

// Phase2 R1: one policy for the local client and server-side validation.
// Package identity is queried from the process token, never from a window
// property, parent-process snapshot, or an executable name on its own.
enum class ExternalClientKind : DWORD {
  None = 0,
  Settings = 1,
  Store = 2,
  Outlook = 3,
  Search = 4
};
constexpr wchar_t kExtCompatibility[] = L"WeaselAcrylicCompatibilityPolicy";
constexpr wchar_t kExtClientKind[] = L"WeaselAcrylicClientKind";
constexpr wchar_t kExtCoordinator[] = L"WeaselAcrylicCoordinatorHwnd";
constexpr wchar_t kExtLowMessages[] = L"WeaselAcrylicExternalLowMessages";
constexpr wchar_t kExtFilterError[] = L"WeaselAcrylicExternalFilterError";
constexpr wchar_t kExtPostError[] = L"WeaselAcrylicExternalPostError";
constexpr wchar_t kExtCreateStage[] = L"WeaselAcrylicCreateStage";
constexpr wchar_t kExtCreateHr[] = L"WeaselAcrylicCreateHresult";
constexpr wchar_t kExtPlacementError[] = L"WeaselAcrylicExternalPlacementError";
constexpr wchar_t kExtPresentationPolicy[] = L"WeaselAcrylicPresentationPolicy";
constexpr wchar_t kExtPresentationReady[] = L"WeaselAcrylicPresentationReady";
constexpr wchar_t kExtPresentationStage[] = L"WeaselAcrylicPresentationStage";
constexpr wchar_t kExtPresentationBlocker[] =
    L"WeaselAcrylicPresentationBlocker";
constexpr wchar_t kExtPresentationRepairs[] =
    L"WeaselAcrylicPresentationRepairs";
constexpr wchar_t kExtPresentationFailClient[] =
    L"WeaselAcrylicPresentationFailClient";
constexpr wchar_t kExtPresentationFailToken[] =
    L"WeaselAcrylicPresentationFailToken";
constexpr wchar_t kExtPresentationBlocked[] =
    L"WeaselAcrylicPresentationBlocked";
constexpr wchar_t kExtPresentationClientStage[] =
    L"WeaselAcrylicPresentationClientStage";
constexpr wchar_t kExtPresentationClientBlocker[] =
    L"WeaselAcrylicPresentationClientBlocker";
constexpr DWORD kPresentationSettleMs = 100;

bool ExternalPathEquals(const std::wstring& left, const std::wstring& right) {
  return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool ExternalImagePath(HANDLE process, std::wstring& path) {
  wchar_t buffer[32768] = {};
  DWORD count = _countof(buffer);
  if (!::QueryFullProcessImageNameW(process, 0, buffer, &count) || !count ||
      count >= _countof(buffer))
    return false;
  path.assign(buffer, count);
  return true;
}

bool ExternalPackageFamily(HANDLE process, std::wstring& family) {
  wchar_t buffer[256] = {};
  UINT32 count = _countof(buffer);
  if (::GetPackageFamilyName(process, &count, buffer) != ERROR_SUCCESS ||
      !count || count > _countof(buffer))
    return false;
  family.assign(buffer);
  return !family.empty();
}

bool ExternalImageInPackage(HANDLE process,
                            const std::wstring& path,
                            const wchar_t* executable) {
  wchar_t package[256] = {};
  UINT32 count = _countof(package);
  if (::GetPackageFullName(process, &count, package) != ERROR_SUCCESS)
    return false;
  wchar_t directory[32768] = {};
  count = _countof(directory);
  if (::GetPackagePathByFullName(package, &count, directory) != ERROR_SUCCESS ||
      !count || count > _countof(directory))
    return false;
  return ExternalPathEquals(path, std::wstring(directory) + L"\\" + executable);
}

ExternalClientKind ExternalKindForProcess(HANDLE process) {
  std::wstring path;
  if (!ExternalImagePath(process, path))
    return ExternalClientKind::None;
  if (IsSettingsImage(path.c_str(), static_cast<DWORD>(path.size())))
    return ExternalClientKind::Settings;
  std::wstring family;
  if (!ExternalPackageFamily(process, family))
    return ExternalClientKind::None;
  if (family == L"Microsoft.WindowsStore_8wekyb3d8bbwe" &&
      ExternalImageInPackage(process, path, L"WinStore.App.exe"))
    return ExternalClientKind::Store;
  if (family == L"Microsoft.OutlookForWindows_8wekyb3d8bbwe") {
    if (ExternalImageInPackage(process, path, L"olk.exe"))
      return ExternalClientKind::Outlook;
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos &&
        ExternalPathEquals(path.substr(slash + 1), L"msedgewebview2.exe"))
      return ExternalClientKind::Outlook;
    // WebView2 is outside the package directory. Its real HWND owner is also
    // checked against the installed olk.exe before a new lease is accepted.
  }
  if (family == L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy" &&
      ExternalImageInPackage(process, path, L"SearchHost.exe"))
    return ExternalClientKind::Search;
  return ExternalClientKind::None;
}

ExternalClientKind CurrentExternalKind() {
#if defined(_M_X64) && !defined(_M_ARM64EC)
  static const ExternalClientKind kind =
      ExternalKindForProcess(::GetCurrentProcess());
  return kind;
#else
  return ExternalClientKind::None;
#endif
}

bool IsExternalCompatibleClient() {
  return CurrentExternalKind() != ExternalClientKind::None;
}

bool ExternalAppContainer(HANDLE process, bool& appContainer) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
    return false;
  DWORD value = 0;
  DWORD bytes = 0;
  const BOOL ok = ::GetTokenInformation(token, TokenIsAppContainer, &value,
                                        sizeof(value), &bytes);
  ::CloseHandle(token);
  if (!ok)
    return false;
  appContainer = value != 0;
  return true;
}

bool ExternalSameUser(HANDLE process) {
  HANDLE ours = nullptr;
  HANDLE theirs = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &ours))
    return false;
  if (!::OpenProcessToken(process, TOKEN_QUERY, &theirs)) {
    ::CloseHandle(ours);
    return false;
  }
  alignas(TOKEN_USER) BYTE a[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
  alignas(TOKEN_USER) BYTE b[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
  DWORD bytes = 0;
  const bool same =
      ::GetTokenInformation(ours, TokenUser, a, sizeof(a), &bytes) &&
      ::GetTokenInformation(theirs, TokenUser, b, sizeof(b), &bytes) &&
      ::EqualSid(reinterpret_cast<TOKEN_USER*>(a)->User.Sid,
                 reinterpret_cast<TOKEN_USER*>(b)->User.Sid);
  ::CloseHandle(theirs);
  ::CloseHandle(ours);
  return same;
}

bool ExternalProcessMatches(DWORD pid, bool client) {
  DWORD currentSession = 0;
  DWORD otherSession = 0;
  if (!pid || pid == ::GetCurrentProcessId() ||
      !::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSession) ||
      !::ProcessIdToSessionId(pid, &otherSession) ||
      currentSession != otherSession)
    return false;
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;
  bool matches = false;
  if (ExternalSameUser(process)) {
    if (client) {
      matches = ExternalKindForProcess(process) != ExternalClientKind::None;
    } else {
      std::wstring path;
      std::wstring root;
      matches = ExternalImagePath(process, path) &&
                SUCCEEDED(ReadAcrylicInstallRoot(root)) &&
                ExternalPathEquals(path, root + L"\\WeaselServer.exe");
    }
  }
  ::CloseHandle(process);
  return matches;
}

bool ExternalMediumServer() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  alignas(TOKEN_MANDATORY_LABEL)
      BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE] = {};
  DWORD bytes = 0;
  bool allowed = false;
  if (::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer),
                            &bytes)) {
    const PSID sid =
        reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    const UCHAR count = *::GetSidSubAuthorityCount(sid);
    allowed = count && *::GetSidSubAuthority(sid, count - 1) ==
                           SECURITY_MANDATORY_MEDIUM_RID;
  }
  ::CloseHandle(token);
  return allowed;
}

void PrepareExternalLowMessages(HWND hwnd) {
  // Only this renderer HWND and two integer/handle-only protocol messages.
  // Never change process-wide filters, UIAccess, UAC, or another app's token.
  DWORD error = ERROR_ACCESS_DENIED;
  bool allowed = false;
  if (ExternalMediumServer()) {
    if (::ChangeWindowMessageFilterEx(hwnd, kExternalUpdate, MSGFLT_ALLOW,
                                      nullptr)) {
      if (::ChangeWindowMessageFilterEx(hwnd, kExternalRelease, MSGFLT_ALLOW,
                                        nullptr)) {
        allowed = true;
        error = ERROR_SUCCESS;
      } else {
        error = ::GetLastError();
        ::ChangeWindowMessageFilterEx(hwnd, kExternalUpdate, MSGFLT_RESET,
                                      nullptr);
      }
    } else {
      error = ::GetLastError();
    }
  }
  SetExternalProperty(hwnd, kExtLowMessages, allowed ? 1 : 0);
  SetExternalProperty(hwnd, kExtFilterError, error);
}

bool ExternalNewClientGeometry(HWND client, const ExternalSnapshot& snap) {
  DWORD clientPid = 0;
  DWORD candidatePid = 0;
  const DWORD clientThread = ::GetWindowThreadProcessId(client, &clientPid);
  const DWORD candidateThread =
      ::GetWindowThreadProcessId(snap.candidate, &candidatePid);
  if (!clientThread || clientThread != candidateThread ||
      clientPid != candidatePid)
    return false;
  const auto exStyle = ::GetWindowLongPtrW(snap.candidate, GWL_EXSTYLE);
  if (!(exStyle & WS_EX_LAYERED) || !(exStyle & WS_EX_NOACTIVATE))
    return false;
  RECT rect = {};
  if (!::GetWindowRect(snap.candidate, &rect) || snap.x < rect.left ||
      snap.y < rect.top ||
      static_cast<LONGLONG>(snap.x) + snap.width > rect.right ||
      static_cast<LONGLONG>(snap.y) + snap.height > rect.bottom)
    return false;
  return true;
}

bool ExternalClientOwnerMatches(HWND client, HWND candidate) {
  DWORD clientPid = 0;
  ::GetWindowThreadProcessId(client, &clientPid);
  // Outlook WebView2's executable path alone is not a sufficient identity.
  HANDLE process =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientPid);
  if (!process)
    return false;
  const auto kind = ExternalKindForProcess(process);
  ::CloseHandle(process);
  if (kind == ExternalClientKind::None)
    return false;
  if (kind != ExternalClientKind::Outlook)
    return true;
  const HWND owner = ::GetWindow(candidate, GW_OWNER);
  if (!owner)
    return false;
  DWORD ownerPid = 0;
  ::GetWindowThreadProcessId(::GetAncestor(owner, GA_ROOT), &ownerPid);
  process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ownerPid);
  if (!process)
    return false;
  std::wstring path;
  std::wstring family;
  const bool matches = ExternalSameUser(process) &&
                       ExternalImagePath(process, path) &&
                       ExternalPackageFamily(process, family) &&
                       family == L"Microsoft.OutlookForWindows_8wekyb3d8bbwe" &&
                       ExternalImageInPackage(process, path, L"olk.exe");
  ::CloseHandle(process);
  return matches;
}

bool IsExternalHostClass(HWND hwnd) {
  wchar_t name[64] = {};
  return hwnd && ::IsWindow(hwnd) &&
         ::GetClassNameW(hwnd, name, _countof(name)) &&
         ::lstrcmpW(name, kWeaselAcrylicBackdropClass) == 0;
}

bool ExternalCandidateVisible(HWND candidate) {
  if (!candidate || !::IsWindow(candidate) || !::IsWindowVisible(candidate))
    return false;
  const HWND owner = ::GetWindow(candidate, GW_OWNER);
  if (owner && (!::IsWindowVisible(owner) || ::IsIconic(owner)))
    return false;
  DWORD cloaked = 0;
  if (owner &&
      SUCCEEDED(::DwmGetWindowAttribute(owner, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
      cloaked)
    return false;
  return true;
}

bool ExternalServerAvailable(HWND hwnd) {
  return IsExternalHostClass(hwnd) &&
         ExternalProperty(hwnd, kExtProtocol) == kExternalProtocol &&
         ExternalProperty(hwnd, kExtServer) == 1 &&
         ExternalProperty(hwnd, kWeaselAcrylicAppSdkActiveProperty) == 1 &&
         ExternalProperty(hwnd, kAcrylicStageProperty) == 100;
}

bool ExternalCoordinatorOnly();

// R2 binds restricted Search/Store clients to their existing input pipe peer.
// This inherits the trust of that input connection; a PID is not a signature.
// The candidate-side route must not OpenProcess/OpenProcessToken on the server.
constexpr wchar_t kExtIdentityRoute[] = L"WeaselAcrylicServerIdentityRoute";
constexpr wchar_t kExtPipeStage[] = L"WeaselAcrylicPipeStage";
constexpr wchar_t kExtPipeError[] = L"WeaselAcrylicPipeError";
constexpr wchar_t kExtPipePid[] = L"WeaselAcrylicPipeServerPid";
constexpr wchar_t kExtPipeSession[] = L"WeaselAcrylicPipeServerSession";
constexpr wchar_t kExtBoundPid[] = L"WeaselAcrylicBoundServerPid";

bool QueryExternalPipeServer(HWND client,
                             ExternalAcrylicState* state,
                             DWORD& processId) {
  processId = 0;
  DWORD sessionId = 0;
  DWORD stage = 11;
  DWORD error = ERROR_NOT_SUPPORTED;
  const bool ok =
      state && state->panel &&
      state->panel->QueryAcrylicServer(processId, sessionId, stage, error);
  SetExternalProperty(client, kExtIdentityRoute, 2);
  SetExternalProperty(client, kExtPipeStage, stage);
  SetExternalProperty(client, kExtPipeError, error);
  SetExternalProperty(client, kExtPipePid, ok ? processId : 0);
  SetExternalProperty(client, kExtPipeSession, ok ? sessionId : 0);
  if (!ok || !processId) {
    processId = 0;
    return false;
  }
  return true;
}

struct ExternalServerSearch {
  HWND window = nullptr;
  DWORD stage = 1;  // 1 no server; 2 policy; 3 filter; 4 legacy identity.
  DWORD pipePid = 0;
  bool pipeBound = false;  // 5 pipe query failed; 6 HWND PID differs from pipe.
};

BOOL CALLBACK FindExternalServerCallback(HWND hwnd, LPARAM parameter) {
  if (!ExternalServerAvailable(hwnd) ||
      (::IsWindowVisible(hwnd) && ExternalProperty(hwnd, kExtActive) != 1))
    return TRUE;
  auto search = reinterpret_cast<ExternalServerSearch*>(parameter);
  if (CurrentExternalKind() != ExternalClientKind::Settings &&
      ExternalProperty(hwnd, kExtCompatibility) != 1) {
    search->stage = 2;
    return TRUE;
  }
  if (ExternalCoordinatorOnly() &&
      ExternalProperty(hwnd, kExtLowMessages) != 1) {
    search->stage = 3;
    return TRUE;
  }
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (search->pipeBound) {
    if (!pid || pid != search->pipePid) {
      search->stage = 6;
      return TRUE;
    }
  } else if (!ExternalProcessMatches(pid, false)) {
    search->stage = 4;
    return TRUE;
  }
  search->window = hwnd;
  search->stage = 0;
  return FALSE;
}

HWND FindExternalServer(HWND client) {
  ExternalServerSearch search;
  auto state = ExternalState(client);
  search.pipeBound = ExternalCoordinatorOnly();
  if (search.pipeBound) {
    if (!QueryExternalPipeServer(client, state, search.pipePid)) {
      SetExternalProperty(client, L"WeaselAcrylicExternalDiscovery", 5);
      return nullptr;  // No fallback to class-name or unverified PID discovery.
    }
  } else {
    SetExternalProperty(client, kExtIdentityRoute, 1);
  }
  ::EnumWindows(FindExternalServerCallback, reinterpret_cast<LPARAM>(&search));
  if (state) {
    state->boundServerPid = search.window ? search.pipePid : 0;
    SetExternalProperty(client, kExtBoundPid, state->boundServerPid);
  }
  SetExternalProperty(client, L"WeaselAcrylicExternalDiscovery", search.stage);
  return search.window;
}

ExternalAcrylicState* MakeExternalState(HWND hwnd, bool server) {
  if (auto state = ExternalState(hwnd))
    return state->server == server ? state : nullptr;
  auto state = new (std::nothrow) ExternalAcrylicState;
  if (!state)
    return nullptr;
  state->server = server;
  ::SetLastError(ERROR_SUCCESS);
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  if (::GetLastError() != ERROR_SUCCESS) {
    delete state;
    return nullptr;
  }
  return state;
}

void PrepareExternalServer(HWND hwnd) {
#if defined(_M_X64) && !defined(_M_ARM64EC)
  if (!MakeExternalState(hwnd, true))
    return;
  PrepareExternalLowMessages(hwnd);
  SetExternalProperty(hwnd, kExtCompatibility, 1);
  SetExternalProperty(hwnd, kExtPresentationPolicy, 1);
  if (!SetExternalProperty(hwnd, kExtProtocol, kExternalProtocol) ||
      !SetExternalProperty(hwnd, kExtServer, 1)) {
    ::RemovePropW(hwnd, kExtProtocol);
    ::RemovePropW(hwnd, kExtServer);
  }
#endif
}

bool ExternalBorrowed(HWND hwnd) {
  const auto state = ExternalState(hwnd);
  return state && state->server && state->active;
}

void EndExternalServerLease(HWND hwnd, ExternalAcrylicState* state) {
  if (!state || !state->server || !state->active)
    return;
  const HWND client = state->peer;
  const DWORD token = state->token;
  state->active = false;
  state->peer = nullptr;
  state->candidate = nullptr;
  ::KillTimer(hwnd, kExternalTimer);
  ::RemovePropW(hwnd, kExtActive);
  ::RemovePropW(hwnd, kExtPresentationReady);
  ::RemovePropW(hwnd, kExtOwner);
  ::RemovePropW(hwnd, kExtAckClient);
  ::RemovePropW(hwnd, kExtAckToken);
  ::RemovePropW(hwnd, kExtAckSequence);
  ::ShowWindow(hwnd, SW_HIDE);
  if (client)
    ::PostMessageW(client, kExternalAck, reinterpret_cast<WPARAM>(hwnd), token);
}

bool ReadExternalSnapshot(HWND client, DWORD token, ExternalSnapshot& snap) {
  const DWORD seq = static_cast<DWORD>(ExternalProperty(client, kExtSequence));
  if (!seq || (seq & 1) ||
      ExternalProperty(client, kExtProtocol) != kExternalProtocol ||
      ExternalProperty(client, kExtToken) != token ||
      ExternalProperty(client, kExtWanted) != 1)
    return false;
  snap.candidate =
      reinterpret_cast<HWND>(ExternalProperty(client, kExtCandidate));
  snap.token = token;
  snap.sequence = seq;
  snap.x =
      static_cast<LONG>(static_cast<DWORD>(ExternalProperty(client, kExtX)));
  snap.y =
      static_cast<LONG>(static_cast<DWORD>(ExternalProperty(client, kExtY)));
  snap.width = static_cast<int>(ExternalProperty(client, kExtWidth));
  snap.height = static_cast<int>(ExternalProperty(client, kExtHeight));
  snap.dark = ExternalProperty(client, kExtDark) ? TRUE : FALSE;
  return ExternalProperty(client, kExtSequence) == seq &&
         ExternalProperty(client, kExtToken) == token &&
         ExternalProperty(client, kExtWanted) == 1 && snap.width > 0 &&
         snap.height > 0 && snap.width <= 32768 && snap.height <= 32768;
}

// Verify the acknowledged rectangle and the currently observable Z-order.
// Hidden windows and non-overlapping windows do not obstruct the material.
// In particular, the server's hidden candidate may legitimately be between the
// foreground and background. IsWindowVisible alone is NOT this check.
DWORD CheckSearchPresentation(HWND host,
                              const ExternalSnapshot& snap,
                              HWND& blocker) {
  blocker = nullptr;
  if (!ExternalCandidateVisible(snap.candidate) || !::IsWindowVisible(host))
    return 10;
  RECT rect = {};
  if (!::GetWindowRect(host, &rect) || rect.left != snap.x ||
      rect.top != snap.y ||
      static_cast<LONGLONG>(rect.right) - rect.left != snap.width ||
      static_cast<LONGLONG>(rect.bottom) - rect.top != snap.height)
    return 20;
  DWORD cloaked = 0;
  if (FAILED(::DwmGetWindowAttribute(host, DWMWA_CLOAKED, &cloaked,
                                     sizeof(cloaked))) ||
      cloaked)
    return 30;
  HWND current = host;
  // Bound the walk because foreign windows can be destroyed/reordered during
  // it. This is conservative structural validation, never a blur certificate.
  for (unsigned i = 0; i < 512; ++i) {
    const HWND previous = ::GetWindow(current, GW_HWNDPREV);
    if (!previous || previous == current)
      return 50;
    if (previous == snap.candidate)
      return 100;
    current = previous;
    if (!::IsWindowVisible(current))
      continue;
    DWORD hidden = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(current, DWMWA_CLOAKED, &hidden,
                                          sizeof(hidden))) &&
        hidden)
      continue;
    RECT other = {};
    if (!::GetWindowRect(current, &other))
      return 60;
    if (other.left < rect.right && other.right > rect.left &&
        other.top < rect.bottom && other.bottom > rect.top) {
      blocker = current;
      return 40;
    }
  }
  return 70;
}

bool SearchProcessForPresentation(DWORD pid) {
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;
  const bool search =
      ExternalKindForProcess(process) == ExternalClientKind::Search;
  ::CloseHandle(process);
  return search;
}

// Run only from the server's existing 100 ms lease timer. No new timer, IPC
// wait, foreground activation, foreign HWND move, or window-band mutation.
void RefreshSearchPresentation(HWND hwnd, ExternalAcrylicState* state) {
  if (!state || !state->server || !state->active || !state->searchPresentation)
    return;
  HWND blocker = nullptr;
  const DWORD stage = CheckSearchPresentation(hwnd, state->snapshot, blocker);
  SetExternalProperty(hwnd, kExtPresentationStage, stage);
  SetExternalProperty(hwnd, kExtPresentationBlocker,
                      reinterpret_cast<ULONG_PTR>(blocker));
  if (stage == 100) {
    if (!state->presentationTiming) {
      state->presentationSince = ::GetTickCount();
      state->presentationTiming = true;
    }
    if (static_cast<DWORD>(::GetTickCount() - state->presentationSince) <
        kPresentationSettleMs) {
      SetExternalProperty(hwnd, kExtPresentationStage, 2);  // Settling.
      return;
    }
    if (ExternalProperty(hwnd, kExtPresentationReady) != 1 &&
        SetExternalProperty(hwnd, kExtPresentationReady, 1))
      ::PostMessageW(state->peer, kExternalAck, reinterpret_cast<WPARAM>(hwnd),
                     state->token);
    return;
  }
  SetExternalProperty(hwnd, kExtPresentationReady, 0);
  state->presentationTiming = false;
  if (!state->presentationRepairUsed) {
    state->presentationRepairUsed = true;
    SetExternalProperty(hwnd, kExtPresentationRepairs, 1);
    const auto& snap = state->snapshot;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL repaired = ::SetWindowPos(
        hwnd, snap.candidate, snap.x, snap.y, snap.width, snap.height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    const DWORD repairError = repaired ? ERROR_SUCCESS : ::GetLastError();
    if (ExternalState(hwnd) != state || repaired)
      return;  // Re-check after another timer tick, not in the same call.
    SetExternalProperty(hwnd, kExtPlacementError, repairError);
  }
  // One unsuccessful repair ends this lease. The matching client records a
  // sticky block until hide/recreate, avoiding an input-driven retry storm.
  SetExternalProperty(hwnd, kExtPresentationFailClient,
                      reinterpret_cast<ULONG_PTR>(state->peer));
  SetExternalProperty(hwnd, kExtPresentationFailToken, state->token);
  EndExternalServerLease(hwnd, state);
}

void ApplyExternalSnapshot(HWND hwnd,
                           ExternalAcrylicState* state,
                           HWND client,
                           DWORD token) {
  ExternalSnapshot snap;
  if (!state || !state->server || !token || !ExternalServerAvailable(hwnd) ||
      !IsExternalHostClass(client) ||
      !ReadExternalSnapshot(client, token, snap) ||
      !ExternalCandidateVisible(snap.candidate) ||
      !ExternalFresh(::GetTickCount(), static_cast<DWORD>(ExternalProperty(
                                           client, kExtClientPulse))))
    return;
  DWORD clientPid = 0;
  DWORD candidatePid = 0;
  ::GetWindowThreadProcessId(client, &clientPid);
  ::GetWindowThreadProcessId(snap.candidate, &candidatePid);
  if (!clientPid || clientPid != candidatePid)
    return;
  const bool compatibilityClient =
      ExternalProperty(client, kExtCompatibility) == 1;
  if (compatibilityClient && !ExternalNewClientGeometry(client, snap))
    return;
  const bool sameLease = state->active && state->peer == client &&
                         state->token == token &&
                         state->candidate == snap.candidate;
  if (!sameLease) {
    if (!ExternalProcessMatches(clientPid, true) ||
        (compatibilityClient &&
         !ExternalClientOwnerMatches(client, snap.candidate)))
      return;
    if (state->active && state->peer != client &&
        ExternalCandidateVisible(state->candidate))
      return;
    if (::IsWindowVisible(hwnd) && !state->active)
      return;  // Never borrow a backdrop displaying the server's own panel.
    EndExternalServerLease(hwnd, state);
    state->active = true;
    state->peer = client;
    state->candidate = snap.candidate;
    state->token = token;
    state->searchPresentation =
        compatibilityClient && SearchProcessForPresentation(clientPid);
    state->presentationTiming = false;
    state->presentationRepairUsed = false;
    SetExternalProperty(hwnd, kExtPresentationReady, 0);
    SetExternalProperty(hwnd, kExtPresentationStage, 1);
    SetExternalProperty(hwnd, kExtPresentationRepairs, 0);
    SetExternalProperty(hwnd, kExtPresentationFailClient, 0);
    SetExternalProperty(hwnd, kExtPresentationFailToken, 0);
    if (!::SetTimer(hwnd, kExternalTimer, 100, nullptr)) {
      EndExternalServerLease(hwnd, state);
      return;
    }
  }
  if (!sameLease || state->snapshot.dark != snap.dark) {
    ::DwmSetWindowAttribute(hwnd, kDwmaUseImmersiveDarkMode, &snap.dark,
                            sizeof(snap.dark));
    g_acrylicAppSdkBridge.SetDarkMode(hwnd, snap.dark);
  }
  ::SetLastError(ERROR_SUCCESS);
  const BOOL positioned = ::SetWindowPos(
      hwnd, snap.candidate, snap.x, snap.y, snap.width, snap.height,
      SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  const DWORD placementError = positioned ? ERROR_SUCCESS : ::GetLastError();
  RECT placed = {};
  const bool placementVerified =
      !compatibilityClient ||
      (positioned && ::GetWindowRect(hwnd, &placed) && placed.left == snap.x &&
       placed.top == snap.y &&
       static_cast<LONGLONG>(placed.right) - placed.left == snap.width &&
       static_cast<LONGLONG>(placed.bottom) - placed.top == snap.height &&
       (state->searchPresentation ||
        ::GetWindow(snap.candidate, GW_HWNDNEXT) == hwnd));
  SetExternalProperty(
      hwnd, kExtPlacementError,
      placementVerified && positioned
          ? ERROR_SUCCESS
          : (placementError ? placementError : ERROR_INVALID_WINDOW_HANDLE));
  if (!positioned || !placementVerified ||
      !SetExternalProperty(hwnd, kExtOwner,
                           reinterpret_cast<ULONG_PTR>(snap.candidate)) ||
      !SetExternalProperty(hwnd, kExtAckClient,
                           reinterpret_cast<ULONG_PTR>(client)) ||
      !SetExternalProperty(hwnd, kExtAckToken, token) ||
      !SetExternalProperty(hwnd, kExtAckSequence, snap.sequence) ||
      !SetExternalProperty(hwnd, kExtServerPulse, ::GetTickCount()) ||
      !SetExternalProperty(hwnd, kExtActive, 1)) {
    EndExternalServerLease(hwnd, state);
    return;
  }
  state->snapshot = snap;
  if (state->searchPresentation) {
    state->presentationTiming = false;
    SetExternalProperty(hwnd, kExtPresentationReady, 0);
    SetExternalProperty(hwnd, kExtPresentationStage, 1);
    return;  // Search ACK is delayed until two stable presentation samples.
  }
  ::PostMessageW(client, kExternalAck, reinterpret_cast<WPARAM>(hwnd), token);
}

bool ExternalLeaseReady(HWND client, const ExternalAcrylicState* state) {
  const HWND host = state ? state->peer : nullptr;
  if (state && state->searchPresentation) {
    HWND blocker = nullptr;
    const DWORD stage =
        host ? CheckSearchPresentation(host, state->snapshot, blocker) : 0;
    SetExternalProperty(client, kExtPresentationClientStage, stage);
    SetExternalProperty(client, kExtPresentationClientBlocker,
                        reinterpret_cast<ULONG_PTR>(blocker));
    if (!host || ExternalProperty(host, kExtPresentationPolicy) != 1 ||
        ExternalProperty(host, kExtPresentationReady) != 1 ||
        ExternalProperty(host, kExtAckSequence) != state->snapshot.sequence ||
        stage != 100)
      return false;
  }
  return state && !state->server && state->active && host &&
         ExternalServerAvailable(host) && ::IsWindowVisible(host) &&
         ExternalProperty(host, kExtActive) == 1 &&
         ExternalProperty(host, kExtAckClient) ==
             reinterpret_cast<ULONG_PTR>(client) &&
         ExternalProperty(host, kExtAckToken) == state->token &&
         ExternalProperty(host, kExtAckSequence) != 0 &&
         ExternalProperty(host, kExtOwner) ==
             reinterpret_cast<ULONG_PTR>(state->candidate) &&
         ExternalFresh(::GetTickCount(), static_cast<DWORD>(ExternalProperty(
                                             host, kExtServerPulse)));
}

void RefreshExternalForeground(HWND hwnd, ExternalAcrylicState* state) {
  if (!state || state->server)
    return;
  const bool ready = ExternalLeaseReady(hwnd, state);
  if (ready == state->foregroundReady)
    return;
  state->foregroundReady = ready;
  if (ready) {
    SetExternalProperty(state->candidate, kExtReady, 1);
    SetExternalProperty(state->candidate, kExtRenderer,
                        reinterpret_cast<ULONG_PTR>(state->peer));
  } else {
    ::RemovePropW(state->candidate, kExtReady);
    ::RemovePropW(state->candidate, kExtRenderer);
  }
  // Some TSF hosts ignore WM_PAINT; use the existing direct redraw entry point.
  if (!state->redrawing && state->panel && ::IsWindow(state->candidate) &&
      ::IsWindowVisible(state->candidate)) {
    state->redrawing = true;
    state->panel->RedrawWindow();
    if (ExternalState(hwnd) == state)
      state->redrawing = false;
  }
}

void ReleaseExternalClient(HWND hwnd) {
  auto state = ExternalState(hwnd);
  if (!state || state->server)
    return;
  ::KillTimer(hwnd, kExternalTimer);
  SetExternalProperty(hwnd, kExtWanted, 0);
  ::RemovePropW(hwnd, kExtToken);  // Reject delayed requests from this lease.
  if (state->peer && state->active)
    ::PostMessageW(state->peer, kExternalRelease,
                   reinterpret_cast<WPARAM>(hwnd), state->token);
  ::RemovePropW(state->candidate, kExtReady);
  ::RemovePropW(state->candidate, kExtRenderer);
  state->active = false;
  state->pending = false;
  state->foregroundReady = false;
  state->attempted = false;
  state->peer = nullptr;
  state->token = 0;
  state->boundServerPid = 0;
  state->presentationBlocked = false;
  SetExternalProperty(hwnd, kExtPresentationBlocked, 0);
  SetExternalProperty(hwnd, kExtBoundPid, 0);
}

DWORD NextExternalToken() {
  static std::atomic<DWORD> counter{::GetTickCount() ^ ::GetCurrentProcessId()};
  DWORD value = counter.fetch_add(1) + 1;
  if (!value)
    value = counter.fetch_add(1) + 1;
  return value;
}

bool PublishExternalSnapshot(HWND hwnd,
                             ExternalAcrylicState* state,
                             const ExternalSnapshot& snap) {
  DWORD seq = state->snapshot.sequence + 2;
  if (!seq)
    seq = 2;
  if (!SetExternalProperty(hwnd, kExtSequence, seq - 1) ||
      !SetExternalProperty(hwnd, kExtProtocol, kExternalProtocol) ||
      !SetExternalProperty(hwnd, kExtCandidate,
                           reinterpret_cast<ULONG_PTR>(state->candidate)) ||
      !SetExternalProperty(hwnd, kExtToken, state->token) ||
      !SetExternalProperty(hwnd, kExtX, static_cast<DWORD>(snap.x)) ||
      !SetExternalProperty(hwnd, kExtY, static_cast<DWORD>(snap.y)) ||
      !SetExternalProperty(hwnd, kExtWidth, snap.width) ||
      !SetExternalProperty(hwnd, kExtHeight, snap.height) ||
      !SetExternalProperty(hwnd, kExtDark, snap.dark ? 1 : 0) ||
      !SetExternalProperty(hwnd, kExtClientPulse, ::GetTickCount()) ||
      !SetExternalProperty(hwnd, kExtWanted, 1) ||
      !SetExternalProperty(hwnd, kExtSequence, seq))
    return false;
  state->snapshot = snap;
  state->snapshot.sequence = seq;
  return true;
}

// A pipe reconnection/server restart invalidates the lease before new requests.
// The callback reads the current TLS handle, not the handle from first attach.
bool ExternalPipePeerCurrent(HWND hwnd, ExternalAcrylicState* state) {
  if (!ExternalCoordinatorOnly())
    return true;
  DWORD pipePid = 0;
  const bool queried = QueryExternalPipeServer(hwnd, state, pipePid);
  DWORD windowPid = 0;
  if (state->peer)
    ::GetWindowThreadProcessId(state->peer, &windowPid);
  if (queried && pipePid == state->boundServerPid && windowPid == pipePid)
    return true;

  SetExternalProperty(hwnd, L"WeaselAcrylicExternalDiscovery", queried ? 6 : 5);
  // Release is asynchronous, and only affects this HWND/token on the server.
  if (state->peer)
    ::PostMessageW(state->peer, kExternalRelease,
                   reinterpret_cast<WPARAM>(hwnd), state->token);
  state->token = NextExternalToken();
  SetExternalProperty(hwnd, kExtToken, state->token);
  state->peer = nullptr;
  state->pending = false;
  state->boundServerPid = 0;
  SetExternalProperty(hwnd, kExtBoundPid, 0);
  state->attempted = false;
  return false;
}

void RequestExternalSnapshot(HWND hwnd, ExternalAcrylicState* state) {
  if (!state || state->server || !state->active || state->presentationBlocked)
    return;
  const DWORD now = ::GetTickCount();
  if (state->peer)
    ExternalPipePeerCurrent(hwnd, state);
  if (state->peer && !ExternalServerAvailable(state->peer)) {
    state->peer = nullptr;
    state->pending = false;
  }
  if (state->pending && ExternalLeaseReady(hwnd, state)) {
    const DWORD acknowledged =
        static_cast<DWORD>(ExternalProperty(state->peer, kExtAckSequence));
    if (static_cast<DWORD>(acknowledged - state->pendingSequence) < 0x80000000u)
      state->pending = false;  // Recover even if the posted ACK was lost.
  }
  if (state->pending) {
    if (static_cast<DWORD>(now - state->pendingSince) <= kExternalTimeoutMs)
      return;
    // Invalidate a late request before trying again; never wait on the input
    // thread.
    const HWND oldPeer = state->peer;
    const DWORD oldToken = state->token;
    state->token = NextExternalToken();
    SetExternalProperty(hwnd, kExtToken, state->token);
    if (oldPeer)
      ::PostMessageW(oldPeer, kExternalRelease, reinterpret_cast<WPARAM>(hwnd),
                     oldToken);
    state->peer = nullptr;
    state->pending = false;
    state->lastAttempt = now;
    state->attempted = true;
  }
  if (!state->peer) {
    if (state->attempted &&
        static_cast<DWORD>(now - state->lastAttempt) < kExternalRetryMs)
      return;
    state->lastAttempt = now;
    state->attempted = true;
    state->peer = FindExternalServer(hwnd);
    if (!state->peer)
      return;
  }
  // The pending request reads the newest published snapshot, so keystrokes and
  // CI #20 moves coalesce instead of queuing one geometry message per change.
  if (ExternalLeaseReady(hwnd, state) &&
      ExternalProperty(state->peer, kExtAckSequence) ==
          state->snapshot.sequence)
    return;
  ::SetLastError(ERROR_SUCCESS);
  if (::PostMessageW(state->peer, kExternalUpdate,
                     reinterpret_cast<WPARAM>(hwnd), state->token)) {
    SetExternalProperty(hwnd, kExtPostError, ERROR_SUCCESS);
    state->pending = true;
    state->pendingSince = now;
    state->pendingSequence = state->snapshot.sequence;
  } else {
    const DWORD error = ::GetLastError();
    SetExternalProperty(hwnd, kExtPostError, error);
    state->peer = nullptr;
    state->lastAttempt = now;
  }
}

void SyncExternalClient(HWND hwnd,
                        HWND candidate,
                        WeaselPanel* panel,
                        int x,
                        int y,
                        int width,
                        int height,
                        BOOL dark) {
  if (!hwnd || !IsExternalCompatibleClient() || width <= 0 || height <= 0 ||
      !ExternalCandidateVisible(candidate))
    return;
  auto state = MakeExternalState(hwnd, false);
  if (!state)
    return;
  state->candidate = candidate;
  state->panel = panel;
  state->searchPresentation =
      CurrentExternalKind() == ExternalClientKind::Search;
  if (CurrentExternalKind() != ExternalClientKind::Settings) {
    SetExternalProperty(hwnd, kExtCompatibility, 1);
    SetExternalProperty(hwnd, kExtClientKind,
                        static_cast<DWORD>(CurrentExternalKind()));
  }
  if (!state->active) {
    state->token = NextExternalToken();
    state->snapshot.sequence = 0;
    state->active = true;
    if (!::SetTimer(hwnd, kExternalTimer, 50, nullptr)) {
      ReleaseExternalClient(hwnd);
      return;
    }
  }
  ExternalSnapshot snap;
  snap.candidate = candidate;
  snap.token = state->token;
  snap.x = x;
  snap.y = y;
  snap.width = width;
  snap.height = height;
  snap.dark = dark;
  const auto& old = state->snapshot;
  if (!old.sequence || old.x != x || old.y != y || old.width != width ||
      old.height != height || old.dark != dark) {
    if (!PublishExternalSnapshot(hwnd, state, snap)) {
      ReleaseExternalClient(hwnd);
      return;
    }
  }
  RequestExternalSnapshot(hwnd, state);
}

bool ExternalForegroundReady(HWND hwnd) {
  const auto state = ExternalState(hwnd);
  return state && state->foregroundReady && ExternalLeaseReady(hwnd, state);
}

void DestroyExternalState(HWND hwnd) {
  auto state = ExternalState(hwnd);
  if (!state)
    return;
  if (state->server)
    EndExternalServerLease(hwnd, state);
  else
    ReleaseExternalClient(hwnd);
  ::KillTimer(hwnd, kExternalTimer);
  ::RemovePropW(hwnd, kExtProtocol);
  ::RemovePropW(hwnd, kExtServer);
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  delete state;
}

bool HandleExternalAcrylicMessage(HWND hwnd,
                                  UINT message,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  LRESULT& result) {
  auto state = ExternalState(hwnd);
  if (!state)
    return false;
  result = 0;
  if (message == WM_NCDESTROY) {
    DestroyExternalState(hwnd);
    return false;
  }
  if (message == kExternalUpdate && state->server) {
    ApplyExternalSnapshot(hwnd, state, reinterpret_cast<HWND>(wParam),
                          static_cast<DWORD>(lParam));
    return true;
  }
  if (message == kExternalRelease && state->server) {
    if (state->peer == reinterpret_cast<HWND>(wParam) &&
        state->token == static_cast<DWORD>(lParam))
      EndExternalServerLease(hwnd, state);
    return true;
  }
  if (message == kExternalAck && !state->server) {
    if (state->peer == reinterpret_cast<HWND>(wParam) &&
        state->token == static_cast<DWORD>(lParam)) {
      state->pending = false;
      if (!ExternalLeaseReady(hwnd, state)) {
        if (state->searchPresentation &&
            ExternalProperty(state->peer, kExtPresentationFailClient) ==
                reinterpret_cast<ULONG_PTR>(hwnd) &&
            ExternalProperty(state->peer, kExtPresentationFailToken) ==
                state->token) {
          state->presentationBlocked = true;
          SetExternalProperty(hwnd, kExtPresentationBlocked, 1);
        }
        // A negative ACK must not create an immediate retry/ACK busy loop.
        state->peer = nullptr;
        state->lastAttempt = ::GetTickCount();
        state->attempted = true;
        state->token = NextExternalToken();
        SetExternalProperty(hwnd, kExtToken, state->token);
      }
      RefreshExternalForeground(hwnd, state);
      if (ExternalState(hwnd) == state)
        RequestExternalSnapshot(hwnd, state);
    }
    return true;
  }
  if (message != WM_TIMER || wParam != kExternalTimer)
    return false;
  if (state->server) {
    if (state->active) {
      const HWND client = state->peer;
      if (!IsExternalHostClass(client) ||
          ExternalProperty(client, kExtToken) != state->token ||
          ExternalProperty(client, kExtWanted) != 1 ||
          !ExternalCandidateVisible(state->candidate) ||
          !ExternalFresh(::GetTickCount(), static_cast<DWORD>(ExternalProperty(
                                               client, kExtClientPulse)))) {
        EndExternalServerLease(hwnd, state);
      } else {
        // Liveness only. Geometry still comes exclusively from panel snapshots.
        SetExternalProperty(hwnd, kExtServerPulse, ::GetTickCount());
        RefreshSearchPresentation(hwnd, state);
      }
    }
  } else if (state->active) {
    if (!ExternalCandidateVisible(state->candidate)) {
      const bool wasReady = state->foregroundReady;
      const HWND candidate = state->candidate;
      WeaselPanel* panel = state->panel;
      ReleaseExternalClient(hwnd);
      // A cloaked/minimized owner can return with the same candidate HWND.
      // Restore its backing pixels now, without reactivating the hidden lease.
      if (wasReady && panel && ::IsWindow(candidate) &&
          ::IsWindowVisible(candidate))
        panel->RedrawWindow();
    } else {
      SetExternalProperty(hwnd, kExtClientPulse, ::GetTickCount());
      RefreshExternalForeground(hwnd, state);
      if (ExternalState(hwnd) == state) {
        RequestExternalSnapshot(hwnd, state);
        RefreshExternalForeground(hwnd, state);
      }
    }
  }
  return true;
}

void SetAcrylicCreationDiagnostic(HWND candidate, LONG stage, HRESULT hr) {
  SetExternalProperty(candidate, kExtCompatibility, 1);
  SetExternalProperty(candidate, kExtClientKind,
                      static_cast<DWORD>(CurrentExternalKind()));
  SetExternalProperty(candidate, kExtCreateStage, static_cast<DWORD>(stage));
  SetExternalProperty(candidate, kExtCreateHr, static_cast<DWORD>(hr));
}

bool ExternalCoordinatorOnly() {
  const auto kind = CurrentExternalKind();
  if (kind != ExternalClientKind::Store && kind != ExternalClientKind::Search)
    return false;
  bool appContainer = false;
  return ExternalAppContainer(::GetCurrentProcess(), appContainer) &&
         appContainer;
}

HWND CreateExternalCoordinator(HWND candidate) {
  // A message-only endpoint performs no DWM/COM initialization and is never
  // displayed. A low-integrity client does not have to create a second visual
  // window owned by the medium-integrity ApplicationFrameHost.
  HWND hwnd =
      ::CreateWindowExW(0, kWeaselAcrylicBackdropClass, L"", 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, AcrylicWindowModule(), nullptr);
  const DWORD error = hwnd ? ERROR_SUCCESS : ::GetLastError();
  SetAcrylicCreationDiagnostic(candidate, hwnd ? 20 : -20,
                               HRESULT_FROM_WIN32(error));
  if (hwnd) {
    SetExternalProperty(candidate, kExtCoordinator,
                        reinterpret_cast<ULONG_PTR>(hwnd));
    SetExternalProperty(hwnd, kExtCandidate,
                        reinterpret_cast<ULONG_PTR>(candidate));
    SetExternalProperty(hwnd, kExtClientKind,
                        static_cast<DWORD>(CurrentExternalKind()));
    SetExternalProperty(hwnd, kExtCompatibility, 1);
  }
  return hwnd;
}

}  // namespace

template <class t0, class t1, class t2>
inline void LoadIconNecessary(t0& a, t1& b, t2& c, int d) {
  if (a == b)
    return;
  a = b;
  if (b.empty())
    c.LoadIconW(d, STATUS_ICON_SIZE, STATUS_ICON_SIZE, LR_DEFAULTCOLOR);
  else
    c = (HICON)LoadImage(NULL, b.c_str(), IMAGE_ICON, STATUS_ICON_SIZE,
                         STATUS_ICON_SIZE, LR_LOADFROMFILE);
}

static inline void ReconfigRoundInfo(IsToRoundStruct& rd,
                                     const int& i,
                                     const int& m_candidateCount) {
  if (i == 0 && m_candidateCount > 1) {
    std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
    std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
  }
  if (i == m_candidateCount - 1) {
    std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
    std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
  }
}

WeaselPanel::WeaselPanel(weasel::UI& ui)
    : m_layout(NULL),
      m_ctx(ui.ctx()),
      m_octx(ui.octx()),
      m_status(ui.status()),
      m_in_server(ui.InServer()),
      m_acrylicServerQuery(ui.acrylicServerQuery()),
      m_style(ui.style()),
      m_ostyle(ui.ostyle()),
      m_candidateCount(0),
      m_lastCandidateCount(0),
      m_current_zhung_icon(),
      m_inputPos(CRect()),
      m_sticky(false),
      dpi(96),
      hide_candidates(false),
      pDWR(ui.pdwr()),
      _UICallback(ui.uiCallback()),
      _m_gdiplusToken(0) {
  m_iconDisabled.LoadIconW(IDI_RELOAD, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                           LR_DEFAULTCOLOR);
  m_iconEnabled.LoadIconW(IDI_ZH, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                          LR_DEFAULTCOLOR);
  m_iconAlpha.LoadIconW(IDI_EN, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                        LR_DEFAULTCOLOR);
  m_iconFull.LoadIconW(IDI_FULL_SHAPE, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                       LR_DEFAULTCOLOR);
  m_iconHalf.LoadIconW(IDI_HALF_SHAPE, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
                       LR_DEFAULTCOLOR);
  // for gdi+ drawings, initialization
  GdiplusStartup(&_m_gdiplusToken, &_m_gdiplusStartupInput, NULL);

  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (hMonitor) {
    GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_hMonitor = hMonitor;
  }
  dpi = dpiX;
  _InitFontRes();
  m_ostyle = m_style;
}

WeaselPanel::~WeaselPanel() {
  _DestroyAcrylicBackdrop();
  // Only full client UI destruction (e.g. TSF Deactivate), not each Esc.
  // This requests asynchronous cleanup on the normal host message loop.
  if (!m_in_server)
    g_acrylicAppSdkBridge.RequestThreadShutdownIfLoaded();
  Gdiplus::GdiplusShutdown(_m_gdiplusToken);
  delete m_layout;
  m_layout = NULL;
  // pDWR.reset();
}

bool WeaselPanel::_CreateAcrylicBackdrop() {
  m_acrylicBackdropEnabled = false;
  SetAcrylicCreationDiagnostic(m_hWnd, 1, S_OK);
  if (!EnsureAcrylicBackdropClass()) {
    SetAcrylicCreationDiagnostic(m_hWnd, -1,
                                 HRESULT_FROM_WIN32(::GetLastError()));
    return false;
  }
  if (!m_in_server && ExternalCoordinatorOnly()) {
    m_acrylicBackdrop = CreateExternalCoordinator(m_hWnd);
    return false;  // No local material; foreground stays opaque until ACK.
  }

  m_acrylicBackdrop = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      kWeaselAcrylicBackdropClass, L"", WS_POPUP, 0, 0, 0, 0,
      ::GetWindow(m_hWnd, GW_OWNER), nullptr, AcrylicWindowModule(), nullptr);

  if (!m_acrylicBackdrop) {
    SetAcrylicCreationDiagnostic(m_hWnd, -2,
                                 HRESULT_FROM_WIN32(::GetLastError()));
    if (!m_in_server && IsExternalCompatibleClient())
      m_acrylicBackdrop = CreateExternalCoordinator(m_hWnd);
    return false;
  }
  SetExternalProperty(m_hWnd, kExtCoordinator,
                      reinterpret_cast<ULONG_PTR>(m_acrylicBackdrop));
  MARGINS margins = {-1, -1, -1, -1};
  const HRESULT frameHr =
      DwmExtendFrameIntoClientArea(m_acrylicBackdrop, &margins);
  if (FAILED(frameHr)) {
    SetAcrylicCreationDiagnostic(m_hWnd, -3, frameHr);
    if (m_in_server || !IsExternalCompatibleClient())
      _DestroyAcrylicBackdrop();
    return false;  // Compatible clients can use this hidden endpoint instead.
  }

  BOOL useHostBackdropBrush = TRUE;
  const HRESULT brushHr = DwmSetWindowAttribute(
      m_acrylicBackdrop, kDwmaUseHostBackdropBrush, &useHostBackdropBrush,
      sizeof(useHostBackdropBrush));
  if (FAILED(brushHr)) {
    SetAcrylicCreationDiagnostic(m_hWnd, -4, brushHr);
    if (m_in_server || !IsExternalCompatibleClient())
      _DestroyAcrylicBackdrop();
    return false;
  }

  int corner = kDwmwcpRound;
  DwmSetWindowAttribute(m_acrylicBackdrop, kDwmaWindowCornerPreference, &corner,
                        sizeof(corner));

  COLORREF borderColor = kDwmColorNone;
  DwmSetWindowAttribute(m_acrylicBackdrop, kDwmaBorderColor, &borderColor,
                        sizeof(borderColor));

  const BOOL useDarkMode = IsDarkColor(m_style.back_color) ? TRUE : FALSE;
  DwmSetWindowAttribute(m_acrylicBackdrop, kDwmaUseImmersiveDarkMode,
                        &useDarkMode, sizeof(useDarkMode));

  // B.2c: both server and TSF client HWNDs use the installed optional helper.
  // The helper resolves the runtime explicitly and owns targets per UI thread.
  // Failure restores the skin and retains diagnostics on a hidden host.
  if (g_acrylicAppSdkBridge.TryInitialize(m_acrylicBackdrop, useDarkMode,
                                          m_in_server)) {
    ::SetPropW(m_acrylicBackdrop, kWeaselAcrylicAppSdkActiveProperty,
               reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    m_acrylicBackdropEnabled = true;
    SetAcrylicCreationDiagnostic(m_hWnd, 100, S_OK);
    if (m_in_server)
      PrepareExternalServer(m_acrylicBackdrop);
    return true;
  }

  // No system-drawn fallback: use the configured skin instead.
  // Leaving m_acrylicBackdropEnabled false preserves the original
  // background alpha, border and shadow in DoPaint.
  // Keep the failed host hidden for Stage/HRESULT inspection; normal
  // window destruction will release it without changing the UI queue.
  SetAcrylicCreationDiagnostic(
      m_hWnd, -5,
      static_cast<HRESULT>(static_cast<DWORD>(
          ExternalProperty(m_acrylicBackdrop, kAcrylicHrProperty))));
  g_acrylicAppSdkBridge.DetachWindow(m_acrylicBackdrop);
  ::ShowWindow(m_acrylicBackdrop, SW_HIDE);
  return false;
}
void WeaselPanel::_DestroyAcrylicBackdrop() {
  RemoveLocalAcrylicGeometry(m_hWnd);
  DestroyExternalState(m_acrylicBackdrop);
  ::RemovePropW(m_hWnd, kExtCoordinator);
  m_acrylicBackdropEnabled = false;
  if (m_acrylicBackdrop) {
    ::RemovePropW(m_acrylicBackdrop, kWeaselAcrylicAppSdkActiveProperty);
    ::ShowWindow(m_acrylicBackdrop, SW_HIDE);
    // Detach this HWND only. Neither unload the DLL nor stop the UI queue.
    g_acrylicAppSdkBridge.DetachWindow(m_acrylicBackdrop);
    ::DestroyWindow(m_acrylicBackdrop);
    m_acrylicBackdrop = NULL;
  }
}
void WeaselPanel::_UpdateAcrylicBackdropTheme() {
  if (!m_acrylicBackdrop)
    return;

  BOOL useDarkMode = IsDarkColor(m_style.back_color) ? TRUE : FALSE;
  DwmSetWindowAttribute(m_acrylicBackdrop, kDwmaUseImmersiveDarkMode,
                        &useDarkMode, sizeof(useDarkMode));

  g_acrylicAppSdkBridge.SetDarkMode(m_acrylicBackdrop, useDarkMode);
}
bool WeaselPanel::_ShouldShowAcrylicBackdrop() const {
  if (!m_acrylicBackdrop || !m_layout || hide_candidates)
    return false;
  if (!m_acrylicBackdropEnabled &&
      (m_in_server || !IsExternalCompatibleClient()))
    return false;

  return ((!m_ctx.empty() && !m_style.inline_preedit) ||
          (m_style.inline_preedit && (m_candidateCount || !m_ctx.aux.empty())));
}

void WeaselPanel::_SyncAcrylicBackdrop() {
  auto localState = LocalAcrylicGeometry(m_hWnd);
  LocalAcrylicGeometryRef lifetime(localState);
  if (DeferLocalAcrylicGeometry(m_hWnd))
    return;
  if (!_ShouldShowAcrylicBackdrop() || !::IsWindowVisible(m_hWnd)) {
    HideAcrylicBackdrop();
    return;
  }

  CRect panelRect;
  GetWindowRect(&panelRect);
  CRect contentRect = m_layout->GetContentRect();

  const int x = panelRect.left + contentRect.left;
  const int y = panelRect.top + contentRect.top;
  const int width = contentRect.Width();
  const int height = contentRect.Height();

  if (width <= 0 || height <= 0) {
    HideAcrylicBackdrop();
    return;
  }

  if (!m_acrylicBackdropEnabled) {
    // Publish the final content rectangle without waiting for another process.
    SyncExternalClient(m_acrylicBackdrop, m_hWnd, this, x, y, width, height,
                       IsDarkColor(m_style.back_color) ? TRUE : FALSE);
    return;
  }

  // A genuinely visible server-side candidate takes priority over a lease.
  // Routine Hide() calls from the server's hidden UI do not revoke the lease.
  if (m_in_server && ExternalBorrowed(m_acrylicBackdrop))
    EndExternalServerLease(m_acrylicBackdrop, ExternalState(m_acrylicBackdrop));
  _UpdateAcrylicBackdropTheme();
  // Theme updates can re-enter the host and destroy/recreate this panel.
  if (localState && !localState->panel)
    return;

  // Skip duplicate geometry/visibility/Z-order work, including notifications
  // caused by our own SetWindowPos. Re-check the HWNDs, not a stale cache.
  if (LocalAcrylicGeometryMatches(m_acrylicBackdrop, m_hWnd, x, y, width,
                                  height))
    return;

  // Keep the DWM Acrylic host immediately below the existing layered
  // Weasel candidate panel.
  ::SetWindowPos(m_acrylicBackdrop, m_hWnd, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

void WeaselPanel::ShowAcrylicBackdrop() {
  auto state = LocalAcrylicGeometry(m_hWnd);
  if (state && !state->syncing) {
    RequestLocalAcrylicGeometry(state);
    return;
  }
  _SyncAcrylicBackdrop();
}

void WeaselPanel::HideAcrylicBackdrop() {
  ReleaseExternalClient(m_acrylicBackdrop);
  if (m_acrylicBackdrop &&
      !(m_in_server && ExternalBorrowed(m_acrylicBackdrop)))
    ::ShowWindow(m_acrylicBackdrop, SW_HIDE);
}

void WeaselPanel::_ResizeWindow() {
  CDCHandle dc = GetDC();
  CSize m_size = m_layout->GetContentSize();
  SetWindowPos(NULL, 0, 0, m_size.cx, m_size.cy,
               SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
  ReleaseDC(dc);
}

void WeaselPanel::_CreateLayout() {
  if (m_layout != NULL)
    delete m_layout;

  Layout* layout = NULL;
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
    layout = new VHorizontalLayout(m_style, m_ctx, m_status, pDWR);
  } else {
    if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL ||
        m_style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
      layout = new VerticalLayout(m_style, m_ctx, m_status, pDWR);
    } else if (m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL ||
               m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN) {
      layout = new HorizontalLayout(m_style, m_ctx, m_status, pDWR);
    }

    if (IS_FULLSCREENLAYOUT(m_style)) {
      layout = new FullScreenLayout(m_style, m_ctx, m_status, m_inputPos,
                                    layout, pDWR);
    }
  }
  m_layout = layout;
}

// 更新界面
void WeaselPanel::Refresh() {
  LocalAcrylicGeometryBatch geometry(m_hWnd);
  bool should_show_icon =
      (m_status.ascii_mode || !m_status.composing || !m_ctx.aux.empty());
  m_candidateCount = min(m_ctx.cinfo.candies.size(), MAX_CANDIDATES_COUNT);
  // When the candidate window changes from having content to having no content,
  // reset the sticky state
  if (m_lastCandidateCount > 0 && m_candidateCount == 0) {
    m_sticky = false;
  }
  m_lastCandidateCount = m_candidateCount;
  // check if to hide candidates window
  // show tips status, two kind of situation: 1) only aux strings, don't care
  // icon status; 2)only icon(ascii mode switching)
  bool show_tips =
      m_in_server &&
      ((!m_ctx.aux.empty() && m_ctx.cinfo.empty() && m_ctx.preedit.empty()) ||
       (m_ctx.empty() && should_show_icon));
  // show schema menu status: schema_id == L".default"
  bool show_schema_menu = m_status.schema_id == L".default";
  bool margin_negative =
      (DPI_SCALE(m_style.margin_x) < 0 || DPI_SCALE(m_style.margin_y) < 0);
  // when to hide_cadidates?
  // 1. margin_negative, and not in show tips mode( ascii switching / half-full
  // switching / simp-trad switching / error tips), and not in schema menu
  // 2. inline preedit without candidates
  bool inline_no_candidates =
      (m_style.inline_preedit && m_candidateCount == 0) && !show_tips;
  hide_candidates = inline_no_candidates ||
                    (margin_negative && !show_tips && !show_schema_menu);

  // only RedrawWindow if no need to hide candidates window, or
  // inline_no_candidates
  if (!hide_candidates || inline_no_candidates) {
    _InitFontRes();
    _CreateLayout();

    CDCHandle dc = GetDC();
    m_layout->DoLayout(dc, pDWR);
    ReleaseDC(dc);
    _ResizeWindow();
    _RepositionWindow();
    if (m_ctx != m_octx) {
      m_octx = m_ctx;
      RedrawWindow();
    }
  }
}

void WeaselPanel::_InitFontRes(bool forced) {
  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (hMonitor)
    GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  // prepare d2d1 resources
  // if style changed, or dpi changed, or pDWR NULL, re-initialize directwrite
  // resources
  if (forced || (pDWR == NULL) || (m_ostyle != m_style) || (dpiX != dpi)) {
    pDWR.reset();
    pDWR = std::make_shared<DirectWriteResources>(m_style, dpiX);
    pDWR->pRenderTarget->SetTextAntialiasMode(
        (D2D1_TEXT_ANTIALIAS_MODE)m_style.antialias_mode);
  }
  m_ostyle = m_style;
  dpi = dpiX;
  dpiScaleLayout = (float)dpi / 96.0f;
}

static HBITMAP CopyDCToBitmap(HDC hDC, LPRECT lpRect) {
  if (!hDC || !lpRect || IsRectEmpty(lpRect))
    return NULL;
  HDC hMemDC = NULL;
  HBITMAP hBitmap = NULL, hOldBitmap = NULL;
  int nX, nY, nX2, nY2;
  int nWidth, nHeight;

  nX = lpRect->left;
  nY = lpRect->top;
  nX2 = lpRect->right;
  nY2 = lpRect->bottom;
  nWidth = nX2 - nX;
  nHeight = nY2 - nY;

  hMemDC = CreateCompatibleDC(hDC);
  if (!hMemDC)
    return NULL;

  hBitmap = CreateCompatibleBitmap(hDC, nWidth, nHeight);
  if (!hBitmap) {
    DeleteDC(hMemDC);
    return NULL;
  }

  hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
  if (!hOldBitmap) {
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    return NULL;
  }

  if (!BitBlt(hMemDC, 0, 0, nWidth, nHeight, hDC, nX, nY, SRCCOPY)) {
    // restore and cleanup
    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    return NULL;
  }

  SelectObject(hMemDC, hOldBitmap);
  DeleteDC(hMemDC);
  return hBitmap;
}

void WeaselPanel::_CaptureRect(CRect& rect) {
  HDC ScreenDC = ::GetDC(NULL);
  CRect rc;
  GetWindowRect(&rc);
  POINT WindowPosAtScreen = {rc.left, rc.top};
  CRect captureRect = rect;
  captureRect.OffsetRect(WindowPosAtScreen);
  // create bitmap first (avoid holding clipboard while capturing)
  HBITMAP bmp = CopyDCToBitmap(ScreenDC, LPRECT(captureRect));
  if (!bmp) {
    ::ReleaseDC(NULL, ScreenDC);
    return;
  }

  // capture input window to clipboard
  if (!OpenClipboard()) {
    DEBUG << "_CaptureRect: OpenClipord ailed";
    DeleteObject(bmp);
    ::ReleaseDC(NULL, ScreenDC);
    return;
  }
  EmptyClipboard();
  if (!SetClipboardData(CF_BITMAP, bmp)) {
    DEBUG << "_CaptureRect: SetClipboardData failed";
    DeleteObject(bmp);
  }
  CloseClipboard();
  ::ReleaseDC(NULL, ScreenDC);
}

LRESULT WeaselPanel::OnMouseActivate(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  bHandled = true;
  return MA_NOACTIVATE;
}

LRESULT WeaselPanel::OnMouseWheel(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  int delta = GET_WHEEL_DELTA_WPARAM(wParam);
  if (_UICallback && delta != 0) {
    bool nextpage = delta < 0;
    _UICallback(NULL, NULL, NULL, &nextpage);
  }
  bHandled = true;
  return 0;
}

LRESULT WeaselPanel::OnLeftClickedUp(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  if (hide_candidates) {
    bHandled = true;
    return 0;
  }
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  ::KillTimer(m_hWnd, AUTOREV_TIMER);
  bar_scale_ = 1.0;
  ptimer = 0;
  {
    // select by click
    CRect rect = m_layout->GetCandidateRect((int)m_ctx.cinfo.highlighted);
    if (m_istorepos)
      rect.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
    rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                     DPI_SCALE(m_style.hilite_padding_y));
    if (rect.PtInRect(point)) {
      size_t i = m_ctx.cinfo.highlighted;
      if (_UICallback) {
        m_mouse_entry = false;
        _UICallback(&i, NULL, NULL, NULL);
        if (!m_status.composing)
          DestroyWindow();
      }
    } else {
      RedrawWindow();
    }
  }
  bHandled = true;
  return 0;
}

LRESULT WeaselPanel::OnLeftClickedDown(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  if (hide_candidates) {
    bHandled = true;
    return 0;
  }
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  // capture
  if (m_style.click_to_capture) {
    CRect recth = m_layout->GetCandidateRect((int)m_ctx.cinfo.highlighted);
    if (m_istorepos)
      recth.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
    recth.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                      DPI_SCALE(m_style.hilite_padding_y));
    // capture widow
    if (recth.PtInRect(point))
      _CaptureRect(recth);
    else {
      // if shadow_color transparent, decrease the capture rectangle size
      if (COLORTRANSPARENT(m_style.shadow_color) &&
          DPI_SCALE(m_style.shadow_radius) != 0) {
        CRect crc(rcw);
        int shadow_gap = (DPI_SCALE(m_style.shadow_offset_x) == 0 &&
                          DPI_SCALE(m_style.shadow_offset_y) == 0)
                             ? 2 * DPI_SCALE(m_style.shadow_radius)
                             : DPI_SCALE(m_style.shadow_radius) +
                                   DPI_SCALE(m_style.shadow_radius) / 2;
        int ofx = DPI_SCALE(m_style.hilite_padding_x) +
                              abs(DPI_SCALE(m_style.shadow_offset_x)) +
                              shadow_gap >
                          abs(DPI_SCALE(m_style.margin_x))
                      ? DPI_SCALE(m_style.hilite_padding_x) +
                            abs(DPI_SCALE(m_style.shadow_offset_x)) +
                            shadow_gap - abs(DPI_SCALE(m_style.margin_x))
                      : 0;
        int ofy = DPI_SCALE(m_style.hilite_padding_y) +
                              abs(DPI_SCALE(m_style.shadow_offset_y)) +
                              shadow_gap >
                          abs(DPI_SCALE(m_style.margin_y))
                      ? DPI_SCALE(m_style.hilite_padding_y) +
                            abs(DPI_SCALE(m_style.shadow_offset_y)) +
                            shadow_gap - abs(DPI_SCALE(m_style.margin_y))
                      : 0;
        crc.DeflateRect(m_layout->offsetX - ofx, m_layout->offsetY - ofy);
        _CaptureRect(crc);
      } else {
        _CaptureRect(rcw);
      }
    }
  }
  {
    if (!m_style.inline_preedit && m_candidateCount != 0 &&
        COLORNOTTRANSPARENT(m_style.prevpage_color) &&
        COLORNOTTRANSPARENT(m_style.nextpage_color)) {
      // click prepage
      if (m_ctx.cinfo.currentPage != 0) {
        CRect prc = m_layout->GetPrepageRect();
        if (m_istorepos)
          prc.OffsetRect(0, m_offsety_preedit);
        if (prc.PtInRect(point)) {
          bool nextPage = false;
          if (_UICallback)
            _UICallback(NULL, NULL, &nextPage, NULL);
          bHandled = true;
          return 0;
        }
      }
      // click nextpage
      if (!m_ctx.cinfo.is_last_page) {
        CRect prc = m_layout->GetNextpageRect();
        if (m_istorepos)
          prc.OffsetRect(0, m_offsety_preedit);
        if (prc.PtInRect(point)) {
          bool nextPage = true;
          if (_UICallback)
            _UICallback(NULL, NULL, &nextPage, NULL);
          bHandled = true;
          return 0;
        }
      }
    }
    // select by click relative actions
    for (size_t i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
      CRect rect = m_layout->GetCandidateRect((int)i);
      if (m_istorepos)
        rect.OffsetRect(0, m_offsetys[i]);
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      if (rect.PtInRect(point)) {
        bar_scale_ = 0.8f;
        // modify highlighted
        if (i != m_ctx.cinfo.highlighted) {
          if (_UICallback)
            _UICallback(NULL, &i, NULL, NULL);
        } else {
          RedrawWindow();
        }
        ptimer = UINT_PTR(this);
        ::SetTimer(m_hWnd, AUTOREV_TIMER, 1000, &WeaselPanel::OnTimer);
        bHandled = true;
        return 0;
      }
    }
  }
  bHandled = true;
  return 0;
}

UINT_PTR WeaselPanel::ptimer = 0;
VOID CALLBACK WeaselPanel::OnTimer(_In_ HWND hwnd,
                                   _In_ UINT uMsg,
                                   _In_ UINT_PTR idEvent,
                                   _In_ DWORD dwTime) {
  ::KillTimer(hwnd, idEvent);
  WeaselPanel* self = (WeaselPanel*)ptimer;
  ptimer = 0;
  if (self) {
    self->bar_scale_ = 1.0;
    self->RedrawWindow();
  }
}

LRESULT WeaselPanel::OnMouseMove(UINT uMsg,
                                 WPARAM wParam,
                                 LPARAM lParam,
                                 BOOL& bHandled) {
  if (m_style.hover_type == UIStyle::NONE)
    return 0;
  if (m_mouse_entry == false) {
    TRACKMOUSEEVENT tme;
    tme.cbSize = sizeof(TRACKMOUSEEVENT);
    tme.dwFlags = TME_LEAVE;
    tme.dwHoverTime = 20;  // unit: ms
    tme.hwndTrack = m_hWnd;
    TrackMouseEvent(&tme);
  }
  bHandled = true;
  m_mouse_entry = true;
  CPoint point;
  point.x = GET_X_LPARAM(lParam);
  point.y = GET_Y_LPARAM(lParam);

  // Ignore if mouse screen position not changed
  CPoint ptScreen = point;
  ClientToScreen(&ptScreen);
  if (ptScreen == m_lastMousePos || m_lastMousePos.x == -1) {
    if (m_lastMousePos.x == -1)
      m_lastMousePos = ptScreen;
    return 0;
  }
  m_lastMousePos = ptScreen;

  for (size_t i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
    CRect rect = m_layout->GetCandidateRect((int)i);
    if (m_istorepos)
      rect.OffsetRect(0, m_offsetys[i]);
    rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                     DPI_SCALE(m_style.hilite_padding_y));
    if (rect.PtInRect(point)) {
      if (i != m_ctx.cinfo.highlighted) {
        if (m_style.hover_type == UIStyle::HoverType::HILITE) {
          if (_UICallback)
            _UICallback(NULL, &i, NULL, NULL);
        } else if (m_hoverIndex != i) {
          m_hoverIndex = static_cast<int>(i);
          InvalidateRect(&rcw, true);
        }
      } else if (m_style.hover_type == UIStyle::HoverType::SEMI_HILITE &&
                 m_hoverIndex != -1) {
        m_hoverIndex = -1;
        InvalidateRect(&rcw, true);
      }
    }
  }
  return 0;
}

LRESULT WeaselPanel::OnMouseLeave(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  m_hoverIndex = -1;
  InvalidateRect(&rcw, true);
  m_mouse_entry = false;
  return 0;
}

void WeaselPanel::_HighlightText(CDCHandle& dc,
                                 const CRect& rc,
                                 const COLORREF& color,
                                 const COLORREF& shadowColor,
                                 const int& radius,
                                 const BackType& type = BackType::TEXT,
                                 const IsToRoundStruct& rd = IsToRoundStruct(),
                                 const COLORREF& bordercolor = TRANS_COLOR) {
  // Graphics obj with SmoothingMode
  Gdiplus::Graphics g_back(dc);
  g_back.SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeHighQuality);

  // blur buffer
  int blurMarginX = m_layout->offsetX;
  int blurMarginY = m_layout->offsetY;

  GraphicsRoundRectPath* hiliteBackPath;
  if (rd.Hemispherical && type != BackType::BACKGROUND &&
      NOT_FULLSCREENLAYOUT(m_style))
    hiliteBackPath = new GraphicsRoundRectPath(
        rc,
        DPI_SCALE(m_style.round_corner_ex) -
            (DPI_SCALE(m_style.border) % 2 ? DPI_SCALE(m_style.border) / 2 : 0),
        rd.IsTopLeftNeedToRound, rd.IsTopRightNeedToRound,
        rd.IsBottomRightNeedToRound, rd.IsBottomLeftNeedToRound);
  else  // background or current candidate background not out of window
        // background
    hiliteBackPath = new GraphicsRoundRectPath(rc, radius);

  // 必须shadow_color都是非完全透明色才做绘制, 全屏状态不绘制阴影保证响应速度
  if (DPI_SCALE(m_style.shadow_radius) && COLORNOTTRANSPARENT(shadowColor) &&
      NOT_FULLSCREENLAYOUT(m_style)) {
    CRect rect(blurMarginX + DPI_SCALE(m_style.shadow_offset_x),
               blurMarginY + DPI_SCALE(m_style.shadow_offset_y),
               rc.Width() + blurMarginX + DPI_SCALE(m_style.shadow_offset_x),
               rc.Height() + blurMarginY + DPI_SCALE(m_style.shadow_offset_y));
    BYTE r = GetRValue(shadowColor);
    BYTE g = GetGValue(shadowColor);
    BYTE b = GetBValue(shadowColor);
    BYTE alpha = (BYTE)((shadowColor >> 24) & 255);
    Gdiplus::Color shadow_color = Gdiplus::Color::MakeARGB(alpha, r, g, b);
    static Gdiplus::Bitmap* pBitmapDropShadow;
    pBitmapDropShadow = new Gdiplus::Bitmap((INT)rc.Width() + blurMarginX * 2,
                                            (INT)rc.Height() + blurMarginY * 2,
                                            PixelFormat32bppPARGB);

    Gdiplus::Graphics g_shadow(pBitmapDropShadow);
    g_shadow.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    // dropshadow, draw a roundrectangle to blur
    if (DPI_SCALE(m_style.shadow_offset_x) != 0 ||
        DPI_SCALE(m_style.shadow_offset_y) != 0) {
      GraphicsRoundRectPath shadow_path(rect, radius);
      Gdiplus::SolidBrush shadow_brush(shadow_color);
      g_shadow.FillPath(&shadow_brush, &shadow_path);
    }
    // round shadow, draw multilines as base round line
    else {
      int step = alpha / DPI_SCALE(m_style.shadow_radius) / 2;
      Gdiplus::Pen pen_shadow(shadow_color, (Gdiplus::REAL)1);
      for (int i = 0; i < DPI_SCALE(m_style.shadow_radius); i++) {
        GraphicsRoundRectPath round_path(rect, radius + 1 + i);
        g_shadow.DrawPath(&pen_shadow, &round_path);
        shadow_color = Gdiplus::Color::MakeARGB(alpha - i * step, r, g, b);
        pen_shadow.SetColor(shadow_color);
        rect.InflateRect(1, 1);
      }
    }
    DoGaussianBlur(pBitmapDropShadow, (float)DPI_SCALE(m_style.shadow_radius),
                   (float)DPI_SCALE(m_style.shadow_radius));

    g_back.DrawImage(pBitmapDropShadow, rc.left - blurMarginX,
                     rc.top - blurMarginY);

    // free memory
    delete pBitmapDropShadow;
    pBitmapDropShadow = NULL;
  }

  // 必须back_color非完全透明才绘制
  if (COLORNOTTRANSPARENT(color)) {
    Gdiplus::Color back_color = GDPCOLOR_FROM_COLORREF(color);
    Gdiplus::SolidBrush back_brush(back_color);
    g_back.FillPath(&back_brush, hiliteBackPath);
  }
  // draw border, for bordercolor not transparent and border valid
  if (COLORNOTTRANSPARENT(bordercolor) && DPI_SCALE(m_style.border) > 0) {
    Gdiplus::Color border_color = GDPCOLOR_FROM_COLORREF(bordercolor);
    Gdiplus::Pen gPenBorder(border_color,
                            (Gdiplus::REAL)DPI_SCALE(m_style.border));
    // candidate window border
    if (type == BackType::BACKGROUND) {
      GraphicsRoundRectPath bgPath(rc, DPI_SCALE(m_style.round_corner_ex));
      g_back.DrawPath(&gPenBorder, &bgPath);
    } else if (type !=
               BackType::TEXT)  // hilited_candidate_border / candidate_border
      g_back.DrawPath(&gPenBorder, hiliteBackPath);
  }
  // free memory
  delete hiliteBackPath;
  hiliteBackPath = NULL;
}

// draw preedit text, text only
bool WeaselPanel::_DrawPreedit(const Text& text,
                               CDCHandle dc,
                               const CRect& rc) {
  bool drawn = false;
  std::wstring const& t = text.str;
  IDWriteTextFormat1* txtFormat = pDWR->pPreeditTextFormat.Get();

  if (!t.empty()) {
    weasel::TextRange range = m_layout->GetPreeditRange();

    if (range.start < range.end) {
      std::wstring before_str = t.substr(0, range.start);
      std::wstring hilited_str = t.substr(range.start, range.end);
      std::wstring after_str = t.substr(range.end);
      CSize beforeSz = m_layout->GetBeforeSize();
      CSize hilitedSz = m_layout->GetHilitedSize();
      CSize afterSz = m_layout->GetAfterSize();

      int x = rc.left;
      int y = rc.top;

      if (range.start > 0) {
        // zzz
        std::wstring str_before(t.substr(0, range.start));
        CRect rc_before;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_before = CRect(rc.left, y, rc.right, y + beforeSz.cy);
        else
          rc_before = CRect(x, rc.top, rc.left + beforeSz.cx, rc.bottom);
        _TextOut(rc_before, str_before.c_str(), str_before.length(),
                 m_style.text_color, txtFormat);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += beforeSz.cy + DPI_SCALE(m_style.hilite_spacing);
        else
          x += beforeSz.cx + DPI_SCALE(m_style.hilite_spacing);
      }
      {
        // zzz[yyy]
        std::wstring str_highlight(
            t.substr(range.start, (size_t)range.end - range.start));
        CRect rc_hi;

        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_hi = CRect(rc.left, y, rc.right, y + hilitedSz.cy);
        else
          rc_hi = CRect(x, rc.top, x + hilitedSz.cx, rc.bottom);
        _TextOut(rc_hi, str_highlight.c_str(), str_highlight.length(),
                 m_style.hilited_text_color, txtFormat);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += rc_hi.Height() + DPI_SCALE(m_style.hilite_spacing);
        else
          x += rc_hi.Width() + DPI_SCALE(m_style.hilite_spacing);
      }
      if (range.end < static_cast<int>(t.length())) {
        // zzz[yyy]xxx
        std::wstring str_after(t.substr(range.end));
        CRect rc_after;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_after = CRect(rc.left, y, rc.right, y + afterSz.cy);
        else
          rc_after = CRect(x, rc.top, x + afterSz.cx, rc.bottom);
        _TextOut(rc_after, str_after.c_str(), str_after.length(),
                 m_style.text_color, txtFormat);
      }
    } else {
      CRect rcText(rc.left, rc.top, rc.right, rc.bottom);
      _TextOut(rcText, t.c_str(), t.length(), m_style.text_color, txtFormat);
    }
    // draw pager mark if not inline_preedit if necessary
    if (m_candidateCount && !m_style.inline_preedit &&
        COLORNOTTRANSPARENT(m_style.prevpage_color) &&
        COLORNOTTRANSPARENT(m_style.nextpage_color)) {
      const std::wstring pre = L"<";
      const std::wstring next = L">";
      CRect prc = m_layout->GetPrepageRect();
      // clickable color / disabled color
      int color =
          m_ctx.cinfo.currentPage ? m_style.prevpage_color : m_style.text_color;
      if (m_istorepos)
        prc.OffsetRect(0, m_offsety_preedit);
      _TextOut(prc, pre.c_str(), pre.length(), color, txtFormat);

      CRect nrc = m_layout->GetNextpageRect();
      // clickable color / disabled color
      color = m_ctx.cinfo.is_last_page ? m_style.text_color
                                       : m_style.nextpage_color;
      if (m_istorepos)
        nrc.OffsetRect(0, m_offsety_preedit);
      _TextOut(nrc, next.c_str(), next.length(), color, txtFormat);
    }
    drawn = true;
  }
  return drawn;
}

// draw hilited back color, back only
bool WeaselPanel::_DrawPreeditBack(const Text& text,
                                   CDCHandle dc,
                                   const CRect& rc) {
  bool drawn = false;
  std::wstring const& t = text.str;
  IDWriteTextFormat1* txtFormat = pDWR->pPreeditTextFormat.Get();

  if (!t.empty()) {
    weasel::TextRange range = m_layout->GetPreeditRange();

    if (range.start < range.end) {
      CSize beforeSz = m_layout->GetBeforeSize();
      CSize hilitedSz = m_layout->GetHilitedSize();

      int x = rc.left;
      int y = rc.top;

      if (range.start > 0) {
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          y += beforeSz.cy + DPI_SCALE(m_style.hilite_spacing);
        else
          x += beforeSz.cx + DPI_SCALE(m_style.hilite_spacing);
      }
      {
        CRect rc_hi;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          rc_hi = CRect(rc.left, y, rc.right, y + hilitedSz.cy);
        else
          rc_hi = CRect(x, rc.top, x + hilitedSz.cx, rc.bottom);
        // if preedit rect size smaller than icon, fill the gap to
        // STATUS_ICON_SIZE
        if (m_layout->ShouldDisplayStatusIcon()) {
          if ((m_style.layout_type == UIStyle::LAYOUT_HORIZONTAL ||
               m_style.layout_type == UIStyle::LAYOUT_VERTICAL) &&
              hilitedSz.cy < STATUS_ICON_SIZE)
            rc_hi.InflateRect(0, (STATUS_ICON_SIZE - hilitedSz.cy) / 2);
          if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT &&
              hilitedSz.cx < STATUS_ICON_SIZE)
            rc_hi.InflateRect((STATUS_ICON_SIZE - hilitedSz.cx) / 2, 0);
        }

        rc_hi.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                          DPI_SCALE(m_style.hilite_padding_y));
        IsToRoundStruct rd = m_layout->GetTextRoundInfo();
        if (m_istorepos) {
          std::swap(rd.IsTopLeftNeedToRound, rd.IsBottomLeftNeedToRound);
          std::swap(rd.IsTopRightNeedToRound, rd.IsBottomRightNeedToRound);
        }
        _HighlightText(dc, rc_hi, m_style.hilited_back_color,
                       m_style.hilited_shadow_color,
                       DPI_SCALE(m_style.round_corner), BackType::TEXT, rd);
      }
    }
    drawn = true;
  }
  return drawn;
}

bool WeaselPanel::_DrawCandidates(CDCHandle& dc, bool back) {
  bool drawn = false;
  const std::vector<Text>& candidates(m_ctx.cinfo.candies);
  const std::vector<Text>& comments(m_ctx.cinfo.comments);
  const std::vector<Text>& labels(m_ctx.cinfo.labels);
  // prevent all text format nullptr
  if (pDWR->pTextFormat.Get() == nullptr &&
      pDWR->pLabelTextFormat.Get() == nullptr &&
      pDWR->pCommentTextFormat.Get() == nullptr) {
    _InitFontRes(true);
  }
  ComPtr<IDWriteTextFormat1> txtFormat = pDWR->pTextFormat;
  ComPtr<IDWriteTextFormat1> labeltxtFormat = pDWR->pLabelTextFormat;
  ComPtr<IDWriteTextFormat1> commenttxtFormat = pDWR->pCommentTextFormat;
  BackType bkType = BackType::CAND;

  CRect rect;
  // draw back color and shadow color, with gdi+
  if (back) {
    // if candidate_shadow_color not transparent, draw candidate shadow first
    if (COLORNOTTRANSPARENT(m_style.candidate_shadow_color)) {
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex)
          continue;  // draw non hilited candidates only
        rect = m_layout->GetCandidateRect((int)i);
        IsToRoundStruct rd = m_layout->GetRoundInfo(i);
        if (m_istorepos) {
          rect.OffsetRect(0, m_offsetys[i]);
          ReconfigRoundInfo(rd, i, m_candidateCount);
        }
        rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                         DPI_SCALE(m_style.hilite_padding_y));
        _HighlightText(dc, rect, 0x00000000, m_style.candidate_shadow_color,
                       DPI_SCALE(m_style.round_corner), bkType, rd);
        drawn = true;
      }
    }
    // draw non highlighted candidates, without shadow
    if ((COLORNOTTRANSPARENT(m_style.candidate_back_color) ||
         COLORNOTTRANSPARENT(m_style.candidate_border_color))) {
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex)
          continue;
        rect = m_layout->GetCandidateRect((int)i);
        IsToRoundStruct rd = m_layout->GetRoundInfo(i);
        if (m_istorepos) {
          rect.OffsetRect(0, m_offsetys[i]);
          ReconfigRoundInfo(rd, i, m_candidateCount);
        }
        rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                         DPI_SCALE(m_style.hilite_padding_y));
        _HighlightText(dc, rect, m_style.candidate_back_color, 0x00000000,
                       DPI_SCALE(m_style.round_corner), bkType, rd,
                       m_style.candidate_border_color);
        drawn = true;
      }
    }
    // draw semi-hilite background and shadow
    if (m_hoverIndex >= 0) {
      rect = m_layout->GetCandidateRect(m_hoverIndex);
      IsToRoundStruct rd = m_layout->GetRoundInfo(m_hoverIndex);
      if (m_istorepos) {
        rect.OffsetRect(0, m_offsetys[m_hoverIndex]);
        ReconfigRoundInfo(rd, m_hoverIndex, m_candidateCount);
      }
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      _HighlightText(dc, rect,
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_back_color),
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_shadow_color),
                     DPI_SCALE(m_style.round_corner), bkType, rd,
                     HALF_ALPHA_COLOR(m_style.hilited_candidate_border_color));
    }
    // draw highlighted background and shadow
    {
      rect = m_layout->GetHighlightRect();
      bool markSt = bar_scale_ == 1.0 || (!m_style.mark_text.empty());
      IsToRoundStruct rd = m_layout->GetRoundInfo(m_ctx.cinfo.highlighted);
      if (m_istorepos) {
        rect.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
        ReconfigRoundInfo(rd, m_ctx.cinfo.highlighted, m_candidateCount);
      }
      rect.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
      _HighlightText(dc, rect, m_style.hilited_candidate_back_color,
                     markSt ? m_style.hilited_candidate_shadow_color : 0,
                     DPI_SCALE(m_style.round_corner), bkType, rd,
                     m_style.hilited_candidate_border_color);
      if (m_style.mark_text.empty() &&
          COLORNOTTRANSPARENT(m_style.hilited_mark_color)) {
        int height =
            min(rect.Height() - DPI_SCALE(m_style.hilite_padding_y) * 2,
                rect.Height() - DPI_SCALE(m_style.round_corner) * 2);
        int width = min(rect.Width() - DPI_SCALE(m_style.hilite_padding_x) * 2,
                        rect.Width() - DPI_SCALE(m_style.round_corner) * 2);
        width = min(width, static_cast<int>(rect.Width() * 0.618));
        height = min(height, static_cast<int>(rect.Height() * 0.618));
        if (bar_scale_ != 1.0f) {
          width = static_cast<int>(width * bar_scale_);
          height = static_cast<int>(height * bar_scale_);
        }
        Gdiplus::Graphics g_back(dc);
        g_back.SetSmoothingMode(
            Gdiplus::SmoothingMode::SmoothingModeHighQuality);
        Gdiplus::Color mark_color =
            GDPCOLOR_FROM_COLORREF(m_style.hilited_mark_color);
        Gdiplus::SolidBrush mk_brush(mark_color);
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
          int x = rect.left + (rect.Width() - width) / 2;
          CRect mkrc{x, rect.top, x + width, rect.top + m_layout->mark_height};
          GraphicsRoundRectPath mk_path(mkrc, mkrc.Height() / 2);
          g_back.FillPath(&mk_brush, &mk_path);
        } else {
          int y = rect.top + (rect.Height() - height) / 2;
          CRect mkrc{rect.left, y, rect.left + m_layout->mark_width,
                     y + height};
          GraphicsRoundRectPath mk_path(mkrc, mkrc.Width() / 2);
          g_back.FillPath(&mk_brush, &mk_path);
        }
      }
      drawn = true;
    }
  }
  // draw text with direct write
  else {
    // begin draw candidate texts
    int label_text_color, candidate_text_color, comment_text_color;
    for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
      if (i == m_ctx.cinfo.highlighted || i == m_hoverIndex) {
        label_text_color = m_style.hilited_label_text_color;
        candidate_text_color = m_style.hilited_candidate_text_color;
        comment_text_color = m_style.hilited_comment_text_color;
      } else {
        label_text_color = m_style.label_text_color;
        candidate_text_color = m_style.candidate_text_color;
        comment_text_color = m_style.comment_text_color;
      }
      // Draw label
      std::wstring label = m_layout->GetLabelText(
          labels, (int)i, m_style.label_text_format.c_str());
      if (!label.empty()) {
        rect = m_layout->GetCandidateLabelRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, label.c_str(), label.length(), label_text_color,
                 labeltxtFormat.Get());
      }
      // Draw text
      std::wstring text = candidates.at(i).str;
      if (!text.empty()) {
        rect = m_layout->GetCandidateTextRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, text.c_str(), text.length(), candidate_text_color,
                 txtFormat.Get());
      }
      // Draw comment
      std::wstring comment = comments.at(i).str;
      if (!comment.empty() && COLORNOTTRANSPARENT(comment_text_color)) {
        rect = m_layout->GetCandidateCommentRect((int)i);
        if (m_istorepos)
          rect.OffsetRect(0, m_offsetys[i]);
        _TextOut(rect, comment.c_str(), comment.length(), comment_text_color,
                 commenttxtFormat.Get());
      }
      drawn = true;
    }
    // draw highlight mark
    {
      if (!m_style.mark_text.empty() &&
          COLORNOTTRANSPARENT(m_style.hilited_mark_color)) {
        CRect rc = m_layout->GetHighlightRect();
        if (m_istorepos)
          rc.OffsetRect(0, m_offsetys[m_ctx.cinfo.highlighted]);
        rc.InflateRect(DPI_SCALE(m_style.hilite_padding_x),
                       DPI_SCALE(m_style.hilite_padding_y));
        int vgap = m_layout->mark_height
                       ? (rc.Height() - m_layout->mark_height) / 2
                       : 0;
        int hgap =
            m_layout->mark_width ? (rc.Width() - m_layout->mark_width) / 2 : 0;
        CRect hlRc;
        if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT)
          hlRc = CRect(rc.left + hgap,
                       rc.top + DPI_SCALE(m_style.hilite_padding_y),
                       rc.left + hgap + m_layout->mark_width,
                       rc.top + DPI_SCALE(m_style.hilite_padding_y) +
                           m_layout->mark_height);
        else
          hlRc = CRect(rc.left + DPI_SCALE(m_style.hilite_padding_x),
                       rc.top + vgap,
                       rc.left + DPI_SCALE(m_style.hilite_padding_x) +
                           m_layout->mark_width,
                       rc.bottom - vgap);
        _TextOut(hlRc, m_style.mark_text.c_str(), m_style.mark_text.length(),
                 m_style.hilited_mark_color, pDWR->pTextFormat.Get());
      }
    }
  }
  return drawn;
}

// draw client area
void WeaselPanel::DoPaint(CDCHandle dc) {
  LocalAcrylicGeometryBatch geometry(m_hWnd);
  // turn off WS_EX_TRANSPARENT, for better resp performance
  ModifyStyleEx(WS_EX_TRANSPARENT, WS_EX_LAYERED);
  GetClientRect(&rcw);
  // prepare memDC
  CDCHandle hdc = ::GetDC(m_hWnd);
  CDCHandle memDC = ::CreateCompatibleDC(hdc);
  HBITMAP memBitmap = ::CreateCompatibleBitmap(hdc, rcw.Width(), rcw.Height());
  ::SelectObject(memDC, memBitmap);
  ReleaseDC(hdc);
  bool drawn = false;
  if (!hide_candidates) {
    CRect auxrc = m_layout->GetAuxiliaryRect();
    CRect preeditrc = m_layout->GetPreeditRect();
    if (m_istorepos) {
      CRect* rects = new CRect[m_candidateCount];
      int* btmys = new int[m_candidateCount];
      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        rects[i] = m_layout->GetCandidateRect(i);
        btmys[i] = rects[i].bottom;
      }
      if (m_candidateCount) {
        if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
          m_offsety_preedit =
              rects[m_candidateCount - 1].bottom - preeditrc.bottom;
        if (!m_ctx.aux.str.empty())
          m_offsety_aux = rects[m_candidateCount - 1].bottom - auxrc.bottom;
      } else {
        m_offsety_preedit = 0;
        m_offsety_aux = 0;
      }
      int base_gap = 0;
      if (!m_ctx.aux.str.empty())
        base_gap = auxrc.Height() + m_style.spacing;
      else if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
        base_gap = preeditrc.Height() + m_style.spacing;

      for (auto i = 0; i < m_candidateCount && i < MAX_CANDIDATES_COUNT; ++i) {
        if (i == 0)
          m_offsetys[i] =
              btmys[m_candidateCount - i - 1] - base_gap - rects[i].bottom;
        else
          m_offsetys[i] = (rects[i - 1].top + m_offsetys[i - 1] -
                           DPI_SCALE(m_style.candidate_spacing)) -
                          rects[i].bottom;
      }
      delete[] rects;
      delete[] btmys;
    }
    // background and candidates back, hilite back drawing start
    if ((!m_ctx.empty() && !m_style.inline_preedit) ||
        (m_style.inline_preedit && (m_candidateCount || !m_ctx.aux.empty()))) {
      CRect backrc = m_layout->GetContentRect();
      COLORREF backColor = m_style.back_color;
      COLORREF shadowColor = m_style.shadow_color;
      const bool localAcrylic =
          m_acrylicBackdropEnabled && !ExternalBorrowed(m_acrylicBackdrop);
      if (localAcrylic || ExternalForegroundReady(m_acrylicBackdrop)) {
        if (COLORNOTTRANSPARENT(backColor))
          backColor = WithAlpha(backColor, kAcrylicTintAlpha);
        shadowColor = TRANS_COLOR;
      }
      _HighlightText(memDC, backrc, backColor, shadowColor,
                     DPI_SCALE(m_style.round_corner_ex), BackType::BACKGROUND,
                     IsToRoundStruct(), m_style.border_color);
    }
    if (!m_ctx.aux.str.empty()) {
      if (m_istorepos)
        auxrc.OffsetRect(0, m_offsety_aux);
      drawn |= _DrawPreeditBack(m_ctx.aux, memDC, auxrc);
    }
    if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty()) {
      if (m_istorepos)
        preeditrc.OffsetRect(0, m_offsety_preedit);
      drawn |= _DrawPreeditBack(m_ctx.preedit, memDC, preeditrc);
    }
    if (m_candidateCount)
      drawn |= _DrawCandidates(memDC, true);
    // background and candidates back, hilite back drawing end

    // begin  texts drawing, if pRenderTarget failed, force to reinit
    // directwrite resources
    if (FAILED(pDWR->pRenderTarget->BindDC(memDC, &rcw))) {
      _InitFontRes(true);
      pDWR->pRenderTarget->BindDC(memDC, &rcw);
    }
    pDWR->pRenderTarget->BeginDraw();
    // draw auxiliary string
    if (!m_ctx.aux.str.empty())
      drawn |= _DrawPreedit(m_ctx.aux, memDC, auxrc);
    // draw preedit string
    if (!m_layout->IsInlinePreedit() && !m_ctx.preedit.str.empty())
      drawn |= _DrawPreedit(m_ctx.preedit, memDC, preeditrc);
    // draw candidates string
    if (m_candidateCount)
      drawn |= _DrawCandidates(memDC);
    if (FAILED(pDWR->pRenderTarget->EndDraw())) {
      _InitFontRes(true);
      Refresh();
    }
    // end texts drawing

    // status icon (I guess Metro IME stole my idea :)
    if (m_layout->ShouldDisplayStatusIcon()) {
      // decide if custom schema zhung icon to show
      LoadIconNecessary(m_current_zhung_icon, m_style.current_zhung_icon,
                        m_iconEnabled, IDI_ZH);
      LoadIconNecessary(m_current_ascii_icon, m_style.current_ascii_icon,
                        m_iconAlpha, IDI_EN);
      LoadIconNecessary(m_current_half_icon, m_style.current_half_icon,
                        m_iconHalf, IDI_HALF_SHAPE);
      LoadIconNecessary(m_current_full_icon, m_style.current_full_icon,
                        m_iconFull, IDI_FULL_SHAPE);
      CRect iconRect(m_layout->GetStatusIconRect());
      if (m_istorepos && !m_ctx.aux.str.empty())
        iconRect.OffsetRect(0, m_offsety_aux);
      else if (m_istorepos && !m_layout->IsInlinePreedit() &&
               !m_ctx.preedit.str.empty())
        iconRect.OffsetRect(0, m_offsety_preedit);

      CIcon& icon(
          m_status.disabled ? m_iconDisabled
          : m_status.ascii_mode
              ? m_iconAlpha
              : (m_status.type == SCHEMA
                     ? m_iconEnabled
                     : (m_status.full_shape ? m_iconFull : m_iconHalf)));
      memDC.DrawIconEx(iconRect.left, iconRect.top, icon, 0, 0);
      drawn = true;
    }
    /* Nothing drawn, hide candidate window */
    if (!drawn) {
      HideAcrylicBackdrop();
      ShowWindow(SW_HIDE);
    }
  }
  _LayerUpdate(rcw, memDC);
  _SyncAcrylicBackdrop();

  // clean objs
  ::DeleteDC(memDC);
  ::DeleteObject(memBitmap);
}

// 由于某些软件并不依赖 WM_PAINT 消息来重绘，在消息循环中直接忽略掉了 WM_PAINT
// 消息， 导致 DoPaint() 永远不会被调用，这里手动调用 DoPaint() 强制重绘
void WeaselPanel::RedrawWindow() {
  HDC hdc = GetDC();
  DoPaint(hdc);
  ReleaseDC(hdc);
}

void WeaselPanel::_LayerUpdate(const CRect& rc, CDCHandle dc) {
  HDC ScreenDC = ::GetDC(NULL);
  CRect rect;
  GetWindowRect(&rect);
  POINT WindowPosAtScreen = {rect.left, rect.top};
  POINT PointOriginal = {0, 0};
  SIZE sz = {rc.Width(), rc.Height()};

  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0XFF, AC_SRC_ALPHA};
  UpdateLayeredWindow(m_hWnd, ScreenDC, &WindowPosAtScreen, &sz, dc,
                      &PointOriginal, RGB(0, 0, 0), &bf, ULW_ALPHA);
  ReleaseDC(ScreenDC);
}

LRESULT WeaselPanel::OnCreate(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  m_mouse_entry = false;
  m_hoverIndex = -1;
  m_acrylicBackdropEnabled = _CreateAcrylicBackdrop();
  if (m_acrylicBackdropEnabled ||
      (m_acrylicBackdrop && !m_in_server && IsExternalCompatibleClient() &&
       CurrentExternalKind() != ExternalClientKind::Settings))
    InstallLocalAcrylicGeometry(m_hWnd, this);
  Refresh();
  return TRUE;
}

LRESULT WeaselPanel::OnDestroy(UINT uMsg,
                               WPARAM wParam,
                               LPARAM lParam,
                               BOOL& bHandled) {
  _DestroyAcrylicBackdrop();
  m_hoverIndex = -1;
  m_lastMousePos = {-1, -1};
  m_sticky = false;
  delete m_layout;
  m_layout = NULL;
  return 0;
}

LRESULT WeaselPanel::OnDpiChanged(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  Refresh();
  return LRESULT();
}

void WeaselPanel::MoveTo(RECT const& rc) {
  LocalAcrylicGeometryBatch geometry(m_hWnd);
  if (!m_layout)
    return;  // avoid handling nullptr in _RepositionWindow
  m_redraw_by_monitor_change = false;
  // The conditions for resetting the sticky state:
  // 1. When the input session ends (ctx.empty() is true)
  // 2. When the input position changes significantly (the position change
  // exceeds the threshold)
  // 3. When the content of the candidate window is empty
  bool should_reset_sticky =
      (m_ctx.empty() || (abs(rc.left - m_inputPos.left) > 50) ||
       (abs(rc.bottom - m_inputPos.bottom) > 50));
  if (should_reset_sticky && m_sticky) {
    m_sticky = false;
    // Force reposition the window
    m_inputPos = rc;
    m_inputPos.OffsetRect(0, 6);
    _RepositionWindow(true);
    RedrawWindow();
    return;
  }
  // if ascii_tip_follow_cursor set, move tip icon to mouse cursor
  if (m_style.ascii_tip_follow_cursor && m_ctx.empty() &&
      (!m_status.composing) && m_layout->ShouldDisplayStatusIcon()) {
    // ascii icon follow cursor
    POINT p;
    ::GetCursorPos(&p);
    RECT irc{p.x - STATUS_ICON_SIZE, p.y - STATUS_ICON_SIZE, p.x, p.y};
    m_inputPos = irc;
    _RepositionWindow(true);
    RedrawWindow();
  } else if (!(rc.left == m_inputPos.left && rc.bottom != m_inputPos.bottom &&
               abs(rc.bottom - m_inputPos.bottom) < 6) ||
             m_layout->ShouldDisplayStatusIcon()) {
    // in some apps like word 2021, with inline_preedit set,
    // bottom of rc would flicker 1 px or 2, make the candidate flickering
    m_inputPos = rc;
    m_inputPos.OffsetRect(0, 6);
    // buffer current m_istorepos status
    bool m_istorepos_buf = m_istorepos;
    // with parameter to avoid vertical flicker
    _RepositionWindow(true);
    // m_istorepos status changed by _RepositionWindow, or tips to show,
    // redrawing is required
    if (m_istorepos != m_istorepos_buf || !m_ctx.aux.empty() ||
        m_layout->ShouldDisplayStatusIcon() || m_redraw_by_monitor_change)
      RedrawWindow();
  }
  // Both local and external Acrylic follow the final candidate position.
  // Local batches defer this until layout/painting is complete; Settings
  // retains the CI #21 asynchronous publish path without owner translation.
  _SyncAcrylicBackdrop();
}

void WeaselPanel::_RepositionWindow(const bool& adj) {
  RECT rcWorkArea;
  memset(&rcWorkArea, 0, sizeof(rcWorkArea));
  HMONITOR hMonitor = MonitorFromRect(m_inputPos, MONITOR_DEFAULTTONEAREST);
  if (hMonitor) {
    MONITORINFO info;
    info.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfo(hMonitor, &info)) {
      rcWorkArea = info.rcWork;
    }
    if (hMonitor != m_hMonitor) {
      m_hMonitor = hMonitor;
      m_redraw_by_monitor_change = true;
    }
  }
  RECT rcWindow;
  GetWindowRect(&rcWindow);
  int width = (rcWindow.right - rcWindow.left);
  int height = (rcWindow.bottom - rcWindow.top);
  // keep panel visible
  rcWorkArea.right -= width;
  rcWorkArea.bottom -= height;
  int x = m_inputPos.left;
  int y = m_inputPos.bottom;
  if (DPI_SCALE(m_style.shadow_radius)) {
    x -= (DPI_SCALE(m_style.shadow_offset_x) >= 0 ||
          COLORTRANSPARENT(m_style.shadow_color))
             ? m_layout->offsetX
             : (m_layout->offsetX / 2);
    if (adj)
      y -= (DPI_SCALE(m_style.shadow_offset_y) > 0 ||
            COLORTRANSPARENT(m_style.shadow_color))
               ? m_layout->offsetY
               : (m_layout->offsetY / 2);
  }
  // for vertical text layout, flow right to left, make window left side
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT &&
      !m_style.vertical_text_left_to_right) {
    x += m_layout->offsetX - width;
    if (DPI_SCALE(m_style.shadow_offset_x) < 0)
      x += m_layout->offsetX;
  }
  if (adj)
    m_istorepos = false;
  if (x > rcWorkArea.right)
    x = rcWorkArea.right;  // over workarea right
  if (x < rcWorkArea.left)
    x = rcWorkArea.left;  // over workarea left
  // show panel above the input focus if we're around the bottom
  if (y > rcWorkArea.bottom || m_sticky) {
    if (!m_sticky)
      m_sticky = true;
    y = m_inputPos.top - height - 6;  // over workarea bottom
    if (DPI_SCALE(m_style.shadow_radius) &&
        DPI_SCALE(m_style.shadow_offset_y) > 0)
      y -= DPI_SCALE(m_style.shadow_offset_y);
    m_istorepos = (m_style.vertical_auto_reverse &&
                   m_style.layout_type == UIStyle::LAYOUT_VERTICAL);
    if (DPI_SCALE(m_style.shadow_radius) > 0)
      y += (DPI_SCALE(m_style.shadow_offset_y) < 0 ||
            COLORTRANSPARENT(m_style.shadow_color))
               ? m_layout->offsetY
               : (m_layout->offsetY / 2);
  }
  if (y < rcWorkArea.top)
    y = rcWorkArea.top;  // over workarea top
  // memorize adjusted position (to avoid window bouncing on height change)
  m_inputPos.bottom = y;
  SetWindowPos(HWND_TOPMOST, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void WeaselPanel::_TextOut(const CRect& rc,
                           const std::wstring& psz,
                           const size_t& cch,
                           const int& inColor,
                           IDWriteTextFormat1* const pTextFormat) {
  if (pTextFormat == NULL)
    return;
  float r = (float)(GetRValue(inColor)) / 255.0f;
  float g = (float)(GetGValue(inColor)) / 255.0f;
  float b = (float)(GetBValue(inColor)) / 255.0f;
  float alpha = (float)((inColor >> 24) & 255) / 255.0f;
  HRESULT hr = S_OK;
  if (pDWR->pBrush == NULL) {
    HR(pDWR->CreateBrush(D2D1::ColorF(r, g, b, alpha)));
  } else
    pDWR->SetBrushColor(D2D1::ColorF(r, g, b, alpha));

  HR(pDWR->CreateTextLayout(psz.c_str(), (int)cch, pTextFormat,
                            (float)rc.Width(), (float)rc.Height()));
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT) {
    DWRITE_FLOW_DIRECTION flow = m_style.vertical_text_left_to_right
                                     ? DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT
                                     : DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT;
    HR(pDWR->SetLayoutReadingDirection(DWRITE_READING_DIRECTION_TOP_TO_BOTTOM));
    HR(pDWR->SetLayoutFlowDirection(flow));
  }

  // offsetx for font glyph over left
  float offsetx = (float)rc.left;
  float offsety = (float)rc.top;
  // prepare for space when first character overhanged
  DWRITE_OVERHANG_METRICS omt;
  HR(pDWR->GetLayoutOverhangMetrics(&omt));
  if (m_style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT && omt.left > 0)
    offsetx += omt.left;
  if (m_style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT && omt.top > 0)
    offsety += omt.top;

  if (pDWR->pTextLayout != NULL) {
    pDWR->DrawTextLayoutAt({offsetx, offsety});
#if 0
    D2D1_RECT_F rectf =  D2D1::RectF(offsetx, offsety, offsetx + rc.Width(), offsety + rc.Height());
    pDWR->DrawRect(&rectf);
#endif
  }
}
