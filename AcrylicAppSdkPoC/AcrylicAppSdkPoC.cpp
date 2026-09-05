#include <windows.h>
#include <dispatcherqueue.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

#include <winrt/base.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "OneCoreUAP.lib")

namespace {

constexpr DWORD kDwmaUseHostBackdropBrush = 17;

enum class AcrylicDiagnosticStage : LONG {
  kNone = 0,
  kEnteredInitialize = 1,
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
};

LONG g_lastStage = static_cast<LONG>(AcrylicDiagnosticStage::kNone);
HRESULT g_lastHresult = S_OK;

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
  g_rootVisual = nullptr;
  g_desktopTarget = nullptr;
  g_compositor = nullptr;

  if (g_systemDispatcherQueueController != nullptr) {
    try {
      g_systemDispatcherQueueController.ShutdownQueueAsync();
    } catch (...) {
    }
  }
  g_systemDispatcherQueueController = nullptr;
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
  ResetBackdropObjects();
}