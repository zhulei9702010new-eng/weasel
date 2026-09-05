#include <windows.h>
#include <appmodel.h>
#include <dispatcherqueue.h>
#include <dwmapi.h>
#include <roapi.h>
#include <windows.ui.composition.interop.h>
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

#include <map>
#include <memory>
#include <new>
#include <string>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "RuntimeObject.lib")
#pragma comment(lib, "OneCoreUAP.lib")

namespace {

// Policy v3: process-wide explicit bootstrap, thread-owned queues, HWND-owned
// targets. No WinRT object has a static/TLS destructor that runs in DllMain.
constexpr DWORD kDwmaUseHostBackdropBrush = 17;
constexpr wchar_t kLifetimeWindowClass[] = L"WeaselAcrylicThreadLifetimeV3";
constexpr UINT_PTR kShutdownTimer = 1;
thread_local LONG t_lastStage = 0;
thread_local HRESULT t_lastHresult = S_OK;
thread_local wchar_t t_lastMessage[512] = {};

void Diagnose(LONG stage, HRESULT hr = S_OK, LPCWSTR message = L"") noexcept {
  t_lastStage = stage;
  t_lastHresult = hr;
  wcsncpy_s(t_lastMessage, message ? message : L"", _TRUNCATE);
}

HRESULT LastWin32Error() noexcept {
  const DWORD error = ::GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

// The bootstrap reference and helper module intentionally live until process
// exit. A TSF plugin must not tear down the host's process-wide runtime graph.
INIT_ONCE g_bootstrapOnce = INIT_ONCE_STATIC_INIT;
HRESULT g_bootstrapResult = E_PENDING;
HMODULE g_helperModule = nullptr;
HMODULE g_bootstrapModule = nullptr;
int g_modulePinAnchor = 0;

BOOL CALLBACK InitializeBootstrapOnce(PINIT_ONCE, PVOID, PVOID*) noexcept {
  try {
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&g_modulePinAnchor),
                              &g_helperModule)) {
      g_bootstrapResult = LastWin32Error();
      return TRUE;
    }

    // Resolve relative to this DLL, NEVER to WINWORD.EXE/the current directory.
    WCHAR modulePath[32768] = {};
    const DWORD length =
        ::GetModuleFileNameW(g_helperModule, modulePath, _countof(modulePath));
    if (!length || length >= _countof(modulePath)) {
      g_bootstrapResult = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
      return TRUE;
    }
    std::wstring path(modulePath, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      g_bootstrapResult = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
      return TRUE;
    }
    path.resize(slash + 1);
    path += L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
    g_bootstrapModule = ::LoadLibraryExW(
        path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_bootstrapModule) {
      g_bootstrapResult = LastWin32Error();
      return TRUE;
    }

    using InitializeFn = HRESULT(WINAPI*)(UINT32, PCWSTR, PACKAGE_VERSION,
                                          MddBootstrapInitializeOptions);
    auto initialize = reinterpret_cast<InitializeFn>(
        ::GetProcAddress(g_bootstrapModule, "MddBootstrapInitialize2"));
    if (!initialize) {
      g_bootstrapResult = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
      return TRUE;
    }
    PACKAGE_VERSION minimum{};
    minimum.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    // None: no fail-fast, debugger break, installation UI, or host restart.
    // A packaged/incompatible host may decline this optional effect.
    g_bootstrapResult = initialize(WINDOWSAPPSDK_RELEASE_MAJORMINOR,
                                   WINDOWSAPPSDK_RELEASE_VERSION_TAG_W, minimum,
                                   MddBootstrapInitializeOptions_None);
  } catch (...) {
    g_bootstrapResult = E_UNEXPECTED;
  }
  return TRUE;
}

struct Target {
  winrt::Windows::UI::Composition::Compositor compositor{nullptr};
  winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget desktop{
      nullptr};
  winrt::Windows::UI::Composition::ContainerVisual root{nullptr};
  winrt::Microsoft::UI::Composition::SystemBackdrops::
      SystemBackdropConfiguration configuration{nullptr};
  winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController
      acrylic{nullptr};
  BOOL dark = FALSE;
  bool pendingDetach = false;

  void Reset() noexcept {
    if (acrylic) {
      try {
        acrylic.Close();
      } catch (...) {
      }
    }
    acrylic = nullptr;
    configuration = nullptr;
    if (desktop) {
      try {
        desktop.Root(nullptr);
      } catch (...) {
      }
      try {
        desktop.Close();
      } catch (...) {
      }
    }
    root = nullptr;
    desktop = nullptr;
    compositor = nullptr;
  }
  ~Target() { Reset(); }
};

struct ThreadState {
  DWORD threadId = ::GetCurrentThreadId();
  bool roOwned = false;
  bool busy = false;
  bool stopping = false;
  bool shutdownRequested = false;
  HWND lifetimeWindow = nullptr;
  HWND legacyTarget = nullptr;
  winrt::Windows::System::DispatcherQueueController ownedQueue{nullptr};
  winrt::Windows::Foundation::IAsyncAction shutdownAction{nullptr};
  std::map<HWND, std::unique_ptr<Target>> targets;
};

// A small thread record is retained until process exit. Explicit UI teardown
// releases its WinRT resources. Abrupt host-thread/process exit is NOT a place
// to execute COM cleanup from TLS destructors/loader-lock callbacks.
thread_local ThreadState* t_state = nullptr;

HRESULT FinishShutdown(ThreadState& state) noexcept;
HRESULT BeginShutdown(ThreadState& state) noexcept;

LRESULT CALLBACK LifetimeWindowProc(HWND hwnd,
                                    UINT message,
                                    WPARAM wParam,
                                    LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto state =
      reinterpret_cast<ThreadState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_TIMER && wParam == kShutdownTimer && state) {
    FinishShutdown(*state);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (state && state->lifetimeWindow == hwnd)
      state->lifetimeWindow = nullptr;
  }
  return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

HRESULT EnsureLifetimeWindow(ThreadState& state) noexcept {
  if (state.lifetimeWindow)
    return S_OK;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.hInstance = g_helperModule;
  wc.lpfnWndProc = LifetimeWindowProc;
  wc.lpszClassName = kLifetimeWindowClass;
  if (!::RegisterClassExW(&wc)) {
    if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return LastWin32Error();
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!::GetClassInfoExW(g_helperModule, kLifetimeWindowClass, &existing) ||
        existing.lpfnWndProc != LifetimeWindowProc)
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  state.lifetimeWindow =
      ::CreateWindowExW(0, kLifetimeWindowClass, L"", 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, g_helperModule, &state);
  return state.lifetimeWindow ? S_OK : LastWin32Error();
}

// This is called by the host's NORMAL message pump (WM_TIMER). No nested
// message pump is introduced during TSF Deactivate or candidate destruction.
HRESULT FinishShutdown(ThreadState& state) noexcept {
  if (!state.stopping || state.busy)
    return S_FALSE;
  state.busy = true;
  HRESULT result = S_OK;
  try {
    if (state.shutdownAction) {
      if (state.shutdownAction.Status() ==
          winrt::Windows::Foundation::AsyncStatus::Started) {
        state.busy = false;
        return S_FALSE;
      }
      state.shutdownAction.GetResults();
      state.shutdownAction = nullptr;
    }
    state.ownedQueue = nullptr;
    if (state.roOwned) {
      ::RoUninitialize();
      state.roOwned = false;
    }
    state.stopping = false;
    state.shutdownRequested = false;
    Diagnose(120);
  } catch (winrt::hresult_error const& error) {
    result = error.code();
    Diagnose(110, result, error.message().c_str());
  } catch (...) {
    result = E_UNEXPECTED;
    Diagnose(110, result);
  }
  if (state.lifetimeWindow) {
    ::KillTimer(state.lifetimeWindow, kShutdownTimer);
    if (SUCCEEDED(result)) {
      HWND window = state.lifetimeWindow;
      state.lifetimeWindow = nullptr;
      ::DestroyWindow(window);
    }
  }
  // On failure, retain the queue/action/pinned module rather than unloading
  // asynchronous code. Subsequent initialization will fall back.
  state.busy = false;
  return result;
}

HRESULT BeginShutdown(ThreadState& state) noexcept {
  if (state.busy || !state.targets.empty())
    return HRESULT_FROM_WIN32(ERROR_BUSY);
  if (state.stopping)
    return S_FALSE;
  if (!state.roOwned)
    return S_OK;
  state.stopping = true;
  Diagnose(110);
  try {
    if (state.ownedQueue)
      state.shutdownAction = state.ownedQueue.ShutdownQueueAsync();
    if (state.shutdownAction &&
        state.shutdownAction.Status() ==
            winrt::Windows::Foundation::AsyncStatus::Started) {
      if (!::SetTimer(state.lifetimeWindow, kShutdownTimer, 25, nullptr))
        return LastWin32Error();
      return S_FALSE;
    }
    return FinishShutdown(state);
  } catch (winrt::hresult_error const& error) {
    Diagnose(110, error.code(), error.message().c_str());
    return error.code();
  } catch (...) {
    Diagnose(110, E_UNEXPECTED);
    return E_UNEXPECTED;
  }
}

void FlushDetachedTargets(ThreadState& state) noexcept {
  // Remove ownership BEFORE releasing COM objects, which may re-enter us.
  for (;;) {
    auto it = state.targets.begin();
    while (it != state.targets.end() && !it->second->pendingDetach)
      ++it;
    if (it == state.targets.end())
      break;
    auto target = std::move(it->second);
    state.targets.erase(it);
    target.reset();
  }
}

struct BusyScope {
  ThreadState& state;
  explicit BusyScope(ThreadState& s) : state(s) { state.busy = true; }
  ~BusyScope() {
    FlushDetachedTargets(state);
    state.busy = false;
    if (state.shutdownRequested && state.targets.empty())
      BeginShutdown(state);
  }
};

HRESULT PrepareThread(ThreadState& state) {
  if (!state.roOwned) {
    const HRESULT hr = ::RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(hr))
      return hr;  // Never change an MTA host's apartment to STA.
    state.roOwned = true;
  }
  Diagnose(6);  // explicit bootstrap, not inside LoadLibrary/DllMain
  if (!::InitOnceExecuteOnce(&g_bootstrapOnce, InitializeBootstrapOnce, nullptr,
                             nullptr))
    return LastWin32Error();
  if (FAILED(g_bootstrapResult))
    return g_bootstrapResult;
  return EnsureLifetimeWindow(state);
}

HRESULT ValidateWindow(HWND hwnd) noexcept {
  DWORD process = 0;
  const DWORD thread = ::GetWindowThreadProcessId(hwnd, &process);
  if (!hwnd || !thread || process != ::GetCurrentProcessId())
    return E_INVALIDARG;
  return thread == ::GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
}

}  // namespace

extern "C" __declspec(dllexport) LONG WINAPI
WeaselAcrylicAppSdkGetLifetimePolicyVersion() {
  return 3;
}
extern "C" __declspec(dllexport) LONG WINAPI WeaselAcrylicAppSdkGetLastStage() {
  return t_lastStage;
}
extern "C" __declspec(dllexport) LONG WINAPI
WeaselAcrylicAppSdkGetLastHresult() {
  return static_cast<LONG>(t_lastHresult);
}
extern "C" __declspec(dllexport) LPCWSTR WINAPI
WeaselAcrylicAppSdkGetLastMessage() {
  return t_lastMessage;
}

extern "C" __declspec(dllexport) BOOL WINAPI
WeaselAcrylicAppSdkAttach(HWND hwnd, BOOL darkMode) {
  Diagnose(1);
  const HRESULT valid = ValidateWindow(hwnd);
  if (FAILED(valid)) {
    Diagnose(1, valid);
    return FALSE;
  }
  if (!t_state)
    t_state = new (std::nothrow) ThreadState;
  if (!t_state) {
    Diagnose(1, E_OUTOFMEMORY);
    return FALSE;
  }
  auto& state = *t_state;
  if (state.stopping && !state.busy)
    FinishShutdown(state);
  if (state.busy || state.stopping) {
    Diagnose(1, HRESULT_FROM_WIN32(ERROR_BUSY));
    return FALSE;
  }
  state.shutdownRequested = false;
  BusyScope guard(state);
  try {
    if (state.targets.find(hwnd) != state.targets.end()) {
      Diagnose(100);
      return TRUE;
    }
    Diagnose(5);
    const HRESULT prepared = PrepareThread(state);
    if (FAILED(prepared)) {
      Diagnose(t_lastStage, prepared);
      return FALSE;
    }
    Diagnose(10);
    using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
    if (!DesktopAcrylicController::IsSupported()) {
      Diagnose(10, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
      return FALSE;
    }
    Diagnose(20);
    BOOL enabled = TRUE;
    winrt::check_hresult(::DwmSetWindowAttribute(
        hwnd, kDwmaUseHostBackdropBrush, &enabled, sizeof(enabled)));

    Diagnose(30);
    auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    if (!queue) {
      DispatcherQueueOptions options{sizeof(DispatcherQueueOptions),
                                     DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
      winrt::check_hresult(::CreateDispatcherQueueController(
          options,
          reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
              winrt::put_abi(state.ownedQueue))));
    }
    auto target = std::make_unique<Target>();
    Diagnose(40);
    target->compositor = winrt::Windows::UI::Composition::Compositor();
    Diagnose(50);
    namespace abi = ABI::Windows::UI::Composition::Desktop;
    auto interop = target->compositor.as<abi::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        hwnd, true,
        reinterpret_cast<abi::IDesktopWindowTarget**>(
            winrt::put_abi(target->desktop))));
    Diagnose(60);
    target->root = target->compositor.CreateContainerVisual();
    target->root.RelativeSizeAdjustment({1.0f, 1.0f});
    target->desktop.Root(target->root);
    Diagnose(70);
    target->configuration = SystemBackdropConfiguration();
    target->configuration.IsInputActive(true);
    target->configuration.Theme(darkMode ? SystemBackdropTheme::Dark
                                         : SystemBackdropTheme::Light);
    target->dark = darkMode;
    Diagnose(80);
    target->acrylic = DesktopAcrylicController();
    target->acrylic.Kind(DesktopAcrylicKind::Base);
    target->acrylic.SetSystemBackdropConfiguration(target->configuration);
    Diagnose(90);
    if (!target->acrylic.SetTarget(
            winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd),
            target->desktop)) {
      Diagnose(90, E_FAIL);
      return FALSE;
    }
    winrt::check_hresult(ValidateWindow(hwnd));
    state.targets.emplace(hwnd, std::move(target));
    Diagnose(100);
    return TRUE;
  } catch (winrt::hresult_error const& error) {
    Diagnose(t_lastStage, error.code(), error.message().c_str());
  } catch (...) {
    Diagnose(t_lastStage, E_UNEXPECTED);
  }
  return FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI
WeaselAcrylicAppSdkIsWindowActive(HWND hwnd) {
  if (!t_state || t_state->stopping)
    return FALSE;
  auto it = t_state->targets.find(hwnd);
  if (it == t_state->targets.end() || it->second->pendingDetach)
    return FALSE;
  try {
    return !it->second->acrylic.IsClosed();
  } catch (...) {
    return FALSE;
  }
}

extern "C" __declspec(dllexport) void WINAPI
WeaselAcrylicAppSdkSetWindowTheme(HWND hwnd, BOOL darkMode) {
  if (!t_state || t_state->busy || t_state->stopping)
    return;
  BusyScope guard(*t_state);
  auto it = t_state->targets.find(hwnd);
  if (it == t_state->targets.end() || it->second->dark == darkMode)
    return;
  try {
    using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
    it->second->configuration.IsInputActive(true);
    it->second->configuration.Theme(darkMode ? SystemBackdropTheme::Dark
                                             : SystemBackdropTheme::Light);
    it->second->dark = darkMode;
  } catch (winrt::hresult_error const& error) {
    Diagnose(70, error.code(), error.message().c_str());
  } catch (...) {
    Diagnose(70, E_UNEXPECTED);
  }
}

extern "C" __declspec(dllexport) void WINAPI
WeaselAcrylicAppSdkDetach(HWND hwnd) {
  if (!t_state)
    return;
  auto it = t_state->targets.find(hwnd);
  if (it == t_state->targets.end())
    return;
  it->second->pendingDetach = true;
  if (t_state->legacyTarget == hwnd)
    t_state->legacyTarget = nullptr;
  if (!t_state->busy) {
    BusyScope guard(*t_state);
  }
}

// TSF calls this only for full UI destruction, not for every Esc or commit.
// Completion is driven by the host's normal message pump, entirely in this
// pinned helper. No callback points into the potentially unloaded TSF DLL.
extern "C" __declspec(dllexport) HRESULT WINAPI
WeaselAcrylicAppSdkRequestThreadShutdown() {
  if (!t_state || !t_state->roOwned)
    return S_OK;
  if (!t_state->targets.empty())
    return HRESULT_FROM_WIN32(ERROR_BUSY);
  t_state->shutdownRequested = true;
  return t_state->busy ? S_FALSE : BeginShutdown(*t_state);
}

// Compatibility entry points for the existing isolated smoke script.
extern "C" __declspec(dllexport) BOOL WINAPI
WeaselAcrylicAppSdkInitialize(HWND hwnd, BOOL darkMode) {
  const BOOL ok = WeaselAcrylicAppSdkAttach(hwnd, darkMode);
  if (ok)
    t_state->legacyTarget = hwnd;
  return ok;
}
extern "C" __declspec(dllexport) BOOL WINAPI WeaselAcrylicAppSdkIsActive() {
  return t_state ? WeaselAcrylicAppSdkIsWindowActive(t_state->legacyTarget)
                 : FALSE;
}
extern "C" __declspec(dllexport) void WINAPI
WeaselAcrylicAppSdkSetDarkMode(BOOL darkMode) {
  if (t_state)
    WeaselAcrylicAppSdkSetWindowTheme(t_state->legacyTarget, darkMode);
}
extern "C" __declspec(dllexport) void WINAPI WeaselAcrylicAppSdkShutdown() {
  if (t_state)
    WeaselAcrylicAppSdkDetach(t_state->legacyTarget);
}

// Server-only final drain. Clients use RequestThreadShutdown above, never a
// nested message pump from inside TSF Deactivate/DllMain.
extern "C" __declspec(dllexport) HRESULT WINAPI
WeaselAcrylicAppSdkShutdownThread(DWORD timeoutMilliseconds) {
  if (!t_state)
    return S_OK;
  auto& state = *t_state;
  if (state.busy || !state.targets.empty())
    return HRESULT_FROM_WIN32(ERROR_BUSY);
  HRESULT hr = WeaselAcrylicAppSdkRequestThreadShutdown();
  if (FAILED(hr))
    return hr;
  const ULONGLONG start = ::GetTickCount64();
  bool sawQuit = false;
  int quitCode = 0;
  while (state.stopping) {
    hr = FinishShutdown(state);
    if (FAILED(hr) || !state.stopping)
      break;
    if (::GetTickCount64() - start >= timeoutMilliseconds) {
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      Diagnose(110, hr);
      break;
    }
    MSG msg{};
    for (int i = 0; i < 64 && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
         ++i) {
      if (msg.message == WM_QUIT) {
        sawQuit = true;
        quitCode = static_cast<int>(msg.wParam);
      } else {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
      }
    }
    if (state.stopping)
      ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
  }
  if (sawQuit)
    ::PostQuitMessage(quitCode);
  return hr;
}
