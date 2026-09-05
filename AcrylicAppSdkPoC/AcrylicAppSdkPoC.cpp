#include <windows.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

#include <string>

#include <winrt/base.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "OneCoreUAP.lib")

namespace {

constexpr DWORD kDwmaUseHostBackdropBrush = 17;
constexpr wchar_t kWindowsAppRuntime18Family[] =
    L"Microsoft.WindowsAppRuntime.1.8_8wekyb3d8bbwe";

std::wstring g_packageDependencyId;
PACKAGEDEPENDENCY_CONTEXT g_packageDependencyContext = nullptr;

winrt::Microsoft::UI::Dispatching::DispatcherQueueController
    g_dispatcherController{nullptr};
winrt::Windows::UI::Composition::Compositor g_compositor{nullptr};
winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget
    g_desktopTarget{nullptr};
winrt::Windows::UI::Composition::ContainerVisual g_rootVisual{nullptr};
winrt::Microsoft::UI::Composition::SystemBackdrops::
    SystemBackdropConfiguration g_backdropConfiguration{nullptr};
winrt::Microsoft::UI::Composition::SystemBackdrops::
    DesktopAcrylicController g_acrylicController{nullptr};

bool AddWindowsAppRuntime18ToPackageGraph() {
  if (g_packageDependencyContext != nullptr)
    return true;

  PACKAGE_VERSION minVersion{};
  PWSTR dependencyId = nullptr;

  HRESULT hr = TryCreatePackageDependency(
      nullptr, kWindowsAppRuntime18Family, minVersion,
      PackageDependencyProcessorArchitectures_None,
      PackageDependencyLifetimeKind_Process, nullptr,
      CreatePackageDependencyOptions_None, &dependencyId);
  if (FAILED(hr) || dependencyId == nullptr)
    return false;

  g_packageDependencyId.assign(dependencyId);
  HeapFree(GetProcessHeap(), 0, dependencyId);

  PWSTR packageFullName = nullptr;
  hr = AddPackageDependency(
      g_packageDependencyId.c_str(), PACKAGE_DEPENDENCY_RANK_DEFAULT,
      AddPackageDependencyOptions_None, &g_packageDependencyContext,
      &packageFullName);

  if (packageFullName != nullptr)
    HeapFree(GetProcessHeap(), 0, packageFullName);

  if (FAILED(hr)) {
    DeletePackageDependency(g_packageDependencyId.c_str());
    g_packageDependencyId.clear();
    g_packageDependencyContext = nullptr;
    return false;
  }

  return true;
}

void RemoveWindowsAppRuntime18FromPackageGraph() {
  if (g_packageDependencyContext != nullptr) {
    RemovePackageDependency(g_packageDependencyContext);
    g_packageDependencyContext = nullptr;
  }

  if (!g_packageDependencyId.empty()) {
    DeletePackageDependency(g_packageDependencyId.c_str());
    g_packageDependencyId.clear();
  }
}

winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget
CreateDesktopWindowTarget(
    HWND hwnd,
    winrt::Windows::UI::Composition::Compositor const& compositor) {
  namespace abi = ABI::Windows::UI::Composition::Desktop;

  auto interop = compositor.as<abi::ICompositorDesktopInterop>();
  winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{
      nullptr};

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

  if (g_dispatcherController != nullptr) {
    try {
      g_dispatcherController.ShutdownQueue();
    } catch (...) {
    }
  }
  g_dispatcherController = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) BOOL __stdcall
WeaselAcrylicAppSdkInitialize(HWND hwnd, BOOL darkMode) {
  try {
    if (hwnd == nullptr)
      return FALSE;

    if (!AddWindowsAppRuntime18ToPackageGraph())
      return FALSE;

    if (!winrt::Microsoft::UI::Composition::SystemBackdrops::
            DesktopAcrylicController::IsSupported()) {
      RemoveWindowsAppRuntime18FromPackageGraph();
      return FALSE;
    }

    BOOL useHostBackdropBrush = TRUE;
    if (FAILED(DwmSetWindowAttribute(
            hwnd, kDwmaUseHostBackdropBrush, &useHostBackdropBrush,
            sizeof(useHostBackdropBrush)))) {
      RemoveWindowsAppRuntime18FromPackageGraph();
      return FALSE;
    }

    auto queue =
        winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    if (queue == nullptr) {
      g_dispatcherController =
          winrt::Microsoft::UI::Dispatching::DispatcherQueueController::
              CreateOnCurrentThread();
    }

    g_compositor = winrt::Windows::UI::Composition::Compositor();
    g_desktopTarget = CreateDesktopWindowTarget(hwnd, g_compositor);

    g_rootVisual = g_compositor.CreateContainerVisual();
    g_rootVisual.RelativeSizeAdjustment({1.0f, 1.0f});
    g_desktopTarget.Root(g_rootVisual);

    g_backdropConfiguration =
        winrt::Microsoft::UI::Composition::SystemBackdrops::
            SystemBackdropConfiguration();
    g_backdropConfiguration.IsInputActive(true);
    g_backdropConfiguration.Theme(
        darkMode
            ? winrt::Microsoft::UI::Composition::SystemBackdrops::
                  SystemBackdropTheme::Dark
            : winrt::Microsoft::UI::Composition::SystemBackdrops::
                  SystemBackdropTheme::Light);

    g_acrylicController =
        winrt::Microsoft::UI::Composition::SystemBackdrops::
            DesktopAcrylicController();
    g_acrylicController.Kind(
        winrt::Microsoft::UI::Composition::SystemBackdrops::
            DesktopAcrylicKind::Base);
    g_acrylicController.SetSystemBackdropConfiguration(
        g_backdropConfiguration);

    auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
    if (!g_acrylicController.SetTarget(windowId, g_desktopTarget)) {
      ResetBackdropObjects();
      RemoveWindowsAppRuntime18FromPackageGraph();
      return FALSE;
    }

    return TRUE;
  } catch (...) {
    ResetBackdropObjects();
    RemoveWindowsAppRuntime18FromPackageGraph();
    return FALSE;
  }
}

extern "C" __declspec(dllexport) void __stdcall
WeaselAcrylicAppSdkSetDarkMode(BOOL darkMode) {
  try {
    if (g_backdropConfiguration == nullptr)
      return;

    g_backdropConfiguration.IsInputActive(true);
    g_backdropConfiguration.Theme(
        darkMode
            ? winrt::Microsoft::UI::Composition::SystemBackdrops::
                  SystemBackdropTheme::Dark
            : winrt::Microsoft::UI::Composition::SystemBackdrops::
                  SystemBackdropTheme::Light);
  } catch (...) {
  }
}

extern "C" __declspec(dllexport) BOOL __stdcall
WeaselAcrylicAppSdkIsActive() {
  try {
    return (g_acrylicController != nullptr &&
            !g_acrylicController.IsClosed())
               ? TRUE
               : FALSE;
  } catch (...) {
    return FALSE;
  }
}

extern "C" __declspec(dllexport) void __stdcall
WeaselAcrylicAppSdkShutdown() {
  ResetBackdropObjects();
  RemoveWindowsAppRuntime18FromPackageGraph();
}