#include <windows.h>
#include <dispatcherqueue.h>
#include <roapi.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

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

constexpr DWORD kDwmaUseHostBackdropBrush = 17;

enum class AcrylicDiagnosticStage : LONG {
  kNone = 0,
  kEnteredInitialize = 1,
  kApartment = 5,
  kIsSupported = 10,
  kHostBackdropBrush = 20,
  kDispatcherQueue = 30,
  kCompositor = 40,
  kDesktopWindowTarget = 50,
  kRootVisual = 60,
  kBackdropConfiguration = 70,
  kAcrylicController = 80,
  kSetTarget = 90,
  kComplete = 100,
  kThreadShutdown = 110,
  kThreadShutdownComplete = 120,
};

LONG g_lastStage = static_cast<LONG>(AcrylicDiagnosticStage::kNone);
HRESULT g_lastHresult = S_OK;

// B.2b lifecycle policy v2: detach windows separately from UI-thread shutdown.
// The helper is pinned before it starts asynchronous/WinRT work.
DWORD g_uiThreadId = 0;
bool g_roInitialized = false;
bool g_runtimeStopping = false;
bool g_shutdownInProgress = false;
int g_modulePinAnchor = 0;
winrt::Windows::Foundation::IAsyncAction g_queueShutdownAction{nullptr};

HRESULT PrepareUiThreadRuntime() {
  const DWORD threadId = ::GetCurrentThreadId();
  if (g_runtimeStopping)
    return HRESULT_FROM_WIN32(ERROR_SHUTDOWN_IN_PROGRESS);
  if (g_uiThreadId && g_uiThreadId != threadId)
    return RPC_E_WRONG_THREAD;
  if (g_uiThreadId)
    return S_OK;

  HMODULE pinnedModule = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&g_modulePinAnchor),
                            &pinnedModule)) {
    const DWORD error = ::GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }

  // CoInitialize(STA) alone is not our WinRT ownership contract. Balance
  // this successful call (including S_FALSE) only at final UI-thread shutdown.
  const HRESULT hr = ::RoInitialize(RO_INIT_SINGLETHREADED);
  if (FAILED(hr))
    return hr;
  g_roInitialized = true;
  g_uiThreadId = threadId;
  return S_OK;
}

void SetDiagnostic(AcrylicDiagnosticStage stage, HRESULT hr = S_OK) {
  g_lastStage = static_cast<LONG>(stage);
  g_lastHresult = hr;
}

winrt::Windows::System::DispatcherQueueController
    g_systemDispatcherQueueController{nullptr};
winrt::Windows::UI::Composition::Compositor g_compositor{nullptr};
winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget g_desktopTarget{
    nullptr};
winrt::Windows::UI::Composition::ContainerVisual g_rootVisual{nullptr};
winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration
    g_backdropConfiguration{nullptr};
winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController
    g_acrylicController{nullptr};

winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget
CreateDesktopWindowTarget(
    HWND hwnd,
    winrt::Windows::UI::Composition::Compositor const& compositor) {
  namespace abi = ABI::Windows::UI::Composition::Desktop;

  auto interop = compositor.as<abi::ICompositorDesktopInterop>();
  winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};

  winrt::check_hresult(interop->CreateDesktopWindowTarget(
      hwnd, true,
      reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(target))));
  return target;
}

void ResetBackdropObjects() {
  if (g_acrylicController != nullptr) {
    try {
      g_acrylicController.Close();
    } catch (...) {
    }
  }
  g_acrylicController = nullptr;
  g_backdropConfiguration = nullptr;

  if (g_desktopTarget != nullptr) {
    try {
      g_desktopTarget.Root(nullptr);
      g_desktopTarget.Close();
    } catch (...) {
    }
  }
  g_rootVisual = nullptr;
  g_desktopTarget = nullptr;
  g_compositor = nullptr;

  // Target lifetime is shorter than UI-thread lifetime. Keep our system queue
  // running across candidate HWND recreation. Only ShutdownThread may stop it.
}

}  // namespace

extern "C"
    __declspec(dllexport) LONG __stdcall WeaselAcrylicAppSdkGetLastStage() {
  return g_lastStage;
}

extern "C"
    __declspec(dllexport) LONG __stdcall WeaselAcrylicAppSdkGetLastHresult() {
  return static_cast<LONG>(g_lastHresult);
}

extern "C" __declspec(dllexport) BOOL __stdcall WeaselAcrylicAppSdkInitialize(
    HWND hwnd,
    BOOL darkMode) {
  SetDiagnostic(AcrylicDiagnosticStage::kEnteredInitialize);

  try {
    if (hwnd == nullptr) {
      SetDiagnostic(AcrylicDiagnosticStage::kEnteredInitialize, E_INVALIDARG);
      return FALSE;
    }

    SetDiagnostic(AcrylicDiagnosticStage::kApartment);
    const HRESULT runtimeHr = PrepareUiThreadRuntime();
    if (FAILED(runtimeHr)) {
      g_lastHresult = runtimeHr;
      return FALSE;
    }
    ResetBackdropObjects();

    SetDiagnostic(AcrylicDiagnosticStage::kIsSupported);
    if (!winrt::Microsoft::UI::Composition::SystemBackdrops::
            DesktopAcrylicController::IsSupported()) {
      SetDiagnostic(AcrylicDiagnosticStage::kIsSupported,
                    HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
      return FALSE;
    }

    SetDiagnostic(AcrylicDiagnosticStage::kHostBackdropBrush);
    BOOL useHostBackdropBrush = TRUE;
    const HRESULT hostBackdropHr = DwmSetWindowAttribute(
        hwnd, kDwmaUseHostBackdropBrush, &useHostBackdropBrush,
        sizeof(useHostBackdropBrush));
    if (FAILED(hostBackdropHr)) {
      SetDiagnostic(AcrylicDiagnosticStage::kHostBackdropBrush, hostBackdropHr);
      return FALSE;
    }

    SetDiagnostic(AcrylicDiagnosticStage::kDispatcherQueue);
    auto systemQueue =
        winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    if (systemQueue == nullptr) {
      DispatcherQueueOptions options{
          sizeof(DispatcherQueueOptions),
          DQTYPE_THREAD_CURRENT,
          DQTAT_COM_STA,
      };

      winrt::Windows::System::DispatcherQueueController controller{nullptr};
      const HRESULT dispatcherHr = ::CreateDispatcherQueueController(
          options,
          reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
              winrt::put_abi(controller)));
      if (FAILED(dispatcherHr)) {
        SetDiagnostic(AcrylicDiagnosticStage::kDispatcherQueue, dispatcherHr);
        return FALSE;
      }

      g_systemDispatcherQueueController = controller;
    }

    SetDiagnostic(AcrylicDiagnosticStage::kCompositor);
    g_compositor = winrt::Windows::UI::Composition::Compositor();

    SetDiagnostic(AcrylicDiagnosticStage::kDesktopWindowTarget);
    g_desktopTarget = CreateDesktopWindowTarget(hwnd, g_compositor);

    SetDiagnostic(AcrylicDiagnosticStage::kRootVisual);
    g_rootVisual = g_compositor.CreateContainerVisual();
    g_rootVisual.RelativeSizeAdjustment({1.0f, 1.0f});
    g_desktopTarget.Root(g_rootVisual);

    SetDiagnostic(AcrylicDiagnosticStage::kBackdropConfiguration);
    g_backdropConfiguration = winrt::Microsoft::UI::Composition::
        SystemBackdrops::SystemBackdropConfiguration();
    g_backdropConfiguration.IsInputActive(true);
    g_backdropConfiguration.Theme(
        darkMode ? winrt::Microsoft::UI::Composition::SystemBackdrops::
                       SystemBackdropTheme::Dark
                 : winrt::Microsoft::UI::Composition::SystemBackdrops::
                       SystemBackdropTheme::Light);

    SetDiagnostic(AcrylicDiagnosticStage::kAcrylicController);
    g_acrylicController = winrt::Microsoft::UI::Composition::SystemBackdrops::
        DesktopAcrylicController();
    g_acrylicController.Kind(winrt::Microsoft::UI::Composition::
                                 SystemBackdrops::DesktopAcrylicKind::Base);
    g_acrylicController.SetSystemBackdropConfiguration(g_backdropConfiguration);

    SetDiagnostic(AcrylicDiagnosticStage::kSetTarget);
    auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
    if (!g_acrylicController.SetTarget(windowId, g_desktopTarget)) {
      SetDiagnostic(AcrylicDiagnosticStage::kSetTarget, E_FAIL);
      ResetBackdropObjects();
      return FALSE;
    }

    SetDiagnostic(AcrylicDiagnosticStage::kComplete);
    return TRUE;
  } catch (winrt::hresult_error const& error) {
    g_lastHresult = error.code();
    ResetBackdropObjects();
    return FALSE;
  } catch (...) {
    g_lastHresult = E_UNEXPECTED;
    ResetBackdropObjects();
    return FALSE;
  }
}

extern "C" __declspec(dllexport) void __stdcall WeaselAcrylicAppSdkSetDarkMode(
    BOOL darkMode) {
  try {
    if (g_backdropConfiguration == nullptr)
      return;

    g_backdropConfiguration.IsInputActive(true);
    g_backdropConfiguration.Theme(
        darkMode ? winrt::Microsoft::UI::Composition::SystemBackdrops::
                       SystemBackdropTheme::Dark
                 : winrt::Microsoft::UI::Composition::SystemBackdrops::
                       SystemBackdropTheme::Light);
  } catch (...) {
  }
}

extern "C" __declspec(dllexport) BOOL __stdcall WeaselAcrylicAppSdkIsActive() {
  try {
    return (g_acrylicController != nullptr && !g_acrylicController.IsClosed())
               ? TRUE
               : FALSE;
  } catch (...) {
    return FALSE;
  }
}

extern "C" __declspec(dllexport) void __stdcall WeaselAcrylicAppSdkShutdown() {
  // Kept for ABI compatibility: policy v2 means window detach, not thread stop.
  if (g_uiThreadId && g_uiThreadId != ::GetCurrentThreadId())
    return;
  ResetBackdropObjects();
}

extern "C" __declspec(dllexport)
LONG __stdcall WeaselAcrylicAppSdkGetLifetimePolicyVersion() {
  return 2;
}

// Call once after the server app/message loop has stopped and before
// CoUninitialize. Window destruction must NOT call this function.
extern "C"
    __declspec(dllexport) HRESULT __stdcall WeaselAcrylicAppSdkShutdownThread(
        DWORD timeoutMilliseconds) {
  if (!g_uiThreadId)
    return S_OK;
  if (g_uiThreadId != ::GetCurrentThreadId())
    return RPC_E_WRONG_THREAD;
  if (g_shutdownInProgress)
    return HRESULT_FROM_WIN32(ERROR_BUSY);

  g_shutdownInProgress = true;
  g_runtimeStopping = true;
  struct ShutdownScope {
    bool sawQuit = false;
    int quitCode = 0;
    ~ShutdownScope() {
      g_shutdownInProgress = false;
      if (sawQuit)
        ::PostQuitMessage(quitCode);
    }
  } scope;

  SetDiagnostic(AcrylicDiagnosticStage::kThreadShutdown);
  try {
    ResetBackdropObjects();

    if (g_systemDispatcherQueueController != nullptr) {
      if (g_queueShutdownAction == nullptr) {
        g_queueShutdownAction =
            g_systemDispatcherQueueController.ShutdownQueueAsync();
      }

      // A current-thread queue needs a running message pump to finish.
      // Blocking on .get() on this STA would risk a deadlock.
      const ULONGLONG started = ::GetTickCount64();
      using winrt::Windows::Foundation::AsyncStatus;
      while (g_queueShutdownAction.Status() == AsyncStatus::Started) {
        if (::GetTickCount64() - started >= timeoutMilliseconds) {
          g_lastHresult = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
          // Keep the action, queue and pinned DLL alive on a timeout. Never
          // claim completion or unload code while callbacks may still exist.
          return g_lastHresult;
        }

        MSG message{};
        // Limit each batch so a busy queue cannot bypass the time limit.
        for (int i = 0;
             i < 64 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
             ++i) {
          if (message.message == WM_QUIT) {
            scope.sawQuit = true;
            scope.quitCode = static_cast<int>(message.wParam);
          } else {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
          }
        }
        if (g_queueShutdownAction.Status() == AsyncStatus::Started) {
          ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
        }
      }
      g_queueShutdownAction.GetResults();
      g_queueShutdownAction = nullptr;
      g_systemDispatcherQueueController = nullptr;
    }

    if (g_roInitialized) {
      ::RoUninitialize();
      g_roInitialized = false;
    }
    SetDiagnostic(AcrylicDiagnosticStage::kThreadShutdownComplete);
    return S_OK;
  } catch (winrt::hresult_error const& error) {
    g_lastHresult = error.code();
    return g_lastHresult;
  } catch (...) {
    g_lastHresult = E_UNEXPECTED;
    return g_lastHresult;
  }
}
