#include "stdafx.h"

#include "WeaselTSF.h"
#include "CandidateList.h"
#include <KeyEvent.h>
#include <math.h>
#include <appmodel.h>

using namespace std;
using namespace weasel;

namespace {
constexpr wchar_t kOwnerFollowClass[] = L"WeaselSettingsOwnerFollowV1";
constexpr UINT_PTR kOwnerFollowTimer = 0x5746;
constexpr UINT kOwnerFollowIntervalMs = 33;
// Phase2 R1: metadata only. Never override BeginUIElement's show decision.
// SearchHost is not fully covered by EnumWindows on Windows 8+; an explicit
// message-only probe lets the observer distinguish host-drawn UI from a TIP
// window that was missed by top-level enumeration.
constexpr wchar_t kAcrylicUiProbeClass[] = L"WeaselAcrylicUiProbe";

bool AcrylicSearchUiProbeEnabled() {
  static const bool enabled = []() {
    wchar_t path[32768] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path))
      return false;
    const std::wstring image(path, length);
    const auto slash = image.find_last_of(L"\\/");
    if (slash == std::wstring::npos ||
        ::lstrcmpiW(image.c_str() + slash + 1, L"SearchHost.exe") != 0)
      return false;
    wchar_t family[256] = {};
    UINT32 count = _countof(family);
    return ::GetPackageFamilyName(::GetCurrentProcess(), &count, family) ==
               ERROR_SUCCESS &&
           ::lstrcmpW(family, L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy") ==
               0;
  }();
  return enabled;
}

LRESULT CALLBACK AcrylicUiProbeProc(HWND hwnd,
                                    UINT message,
                                    WPARAM wp,
                                    LPARAM lp) {
  return ::DefWindowProcW(hwnd, message, wp, lp);
}

void UpdateAcrylicUiProbe(const void* instance,
                          DWORD stage,
                          HRESULT hr,
                          BOOL show) {
  if (!AcrylicSearchUiProbeEnabled())
    return;
  struct Probe {
    HWND window = nullptr;
    const void* instance = nullptr;  // Not published outside this thread.
  };
  static thread_local Probe probe;
  if (stage == 1)
    probe.instance = instance;
  if (probe.instance != instance)
    return;
  if (!::IsWindow(probe.window)) {
    HMODULE module = nullptr;
    static int anchor = 0;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&anchor), &module))
      return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = module;
    wc.lpfnWndProc = AcrylicUiProbeProc;
    wc.lpszClassName = kAcrylicUiProbeClass;
    if (!::RegisterClassExW(&wc)) {
      WNDCLASSEXW existing = {};
      existing.cbSize = sizeof(existing);
      if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
          !::GetClassInfoExW(module, kAcrylicUiProbeClass, &existing) ||
          existing.lpfnWndProc != AcrylicUiProbeProc)
        return;
    }
    // Windows destroys this thread-owned HWND on thread exit. Keeping one
    // per UI thread avoids changing COM object or candidate lifetimes.
    probe.window = ::CreateWindowExW(0, kAcrylicUiProbeClass, L"", 0, 0, 0, 0,
                                     0, HWND_MESSAGE, nullptr, module, nullptr);
  }
  if (!probe.window)
    return;
  ::SetPropW(probe.window, L"WeaselAcrylicUiProbeVersion",
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)));
  ::SetPropW(probe.window, L"WeaselAcrylicUiProbeStage",
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(stage)));
  ::SetPropW(
      probe.window, L"WeaselAcrylicUiProbeHresult",
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(static_cast<DWORD>(hr))));
  const DWORD policy = (stage == 1 || FAILED(hr)) ? 0 : (show ? 1 : 2);
  ::SetPropW(probe.window, L"WeaselAcrylicUiShowPolicy",
             reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(policy)));
  ::SetPropW(
      probe.window, L"WeaselAcrylicUiProbePulse",
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(::GetTickCount())));
  if (stage == 3)
    probe.instance = nullptr;
}

}  // namespace

CCandidateList::CCandidateList(com_ptr<WeaselTSF> pTextService)
    : _settingsFollowEnabled(_IsSettingsHost()),
      _ui(make_unique<UI>()),
      _tsf(pTextService),
      _pbShow(TRUE) {
  _cRef = 1;
  _ui->SetAcrylicServerQuery(
      [this](DWORD& processId, DWORD& sessionId, DWORD& stage, DWORD& error) {
        return _tsf->QueryAcrylicServer(processId, sessionId, stage, error);
      });
}

CCandidateList::~CCandidateList() {
  // _tsf is destroyed before _ui; do not retain a callback to this object.
  _ui->SetAcrylicServerQuery({});
  UpdateAcrylicUiProbe(this, 3, S_OK, _pbShow);
  _StopOwnerFollow(true);
}

STDMETHODIMP CCandidateList::QueryInterface(REFIID riid, void** ppvObj) {
  if (ppvObj == nullptr) {
    return E_INVALIDARG;
  }

  *ppvObj = nullptr;

  if (IsEqualIID(riid, IID_ITfUIElement) ||
      IsEqualIID(riid, IID_ITfCandidateListUIElement) ||
      IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior)) {
    *ppvObj = (ITfCandidateListUIElementBehavior*)this;
  } else if (IsEqualIID(riid, IID_IUnknown) ||
             IsEqualIID(riid,
                        __uuidof(ITfIntegratableCandidateListUIElement))) {
    *ppvObj = (ITfIntegratableCandidateListUIElement*)this;
  }

  if (*ppvObj) {
    AddRef();
    return S_OK;
  }

  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CCandidateList::AddRef(void) {
  return ++_cRef;
}

STDMETHODIMP_(ULONG) CCandidateList::Release(void) {
  LONG cr = --_cRef;

  assert(_cRef >= 0);

  if (_cRef == 0) {
    delete this;
  }

  return cr;
}

STDMETHODIMP CCandidateList::GetDescription(BSTR* pbstr) {
  static auto str = SysAllocString(L"Candidate List");
  if (pbstr) {
    *pbstr = str;
  }
  return S_OK;
}

STDMETHODIMP CCandidateList::GetGUID(GUID* pguid) {
  /// 36c3c795-7159-45aa-ab12-30229a51dbd3
  *pguid = {0x36c3c795,
            0x7159,
            0x45aa,
            {0xab, 0x12, 0x30, 0x22, 0x9a, 0x51, 0xdb, 0xd3}};
  return S_OK;
}

STDMETHODIMP CCandidateList::Show(BOOL showCandidateWindow) {
  if (showCandidateWindow) {
    _ui->Show();
    _StartOwnerFollow();
  } else {
    _StopOwnerFollow(false);
    _ui->Hide();
  }
  return S_OK;
}

STDMETHODIMP CCandidateList::IsShown(BOOL* pIsShow) {
  *pIsShow = _ui->IsShown();
  return S_OK;
}

STDMETHODIMP CCandidateList::GetUpdatedFlags(DWORD* pdwFlags) {
  if (!pdwFlags)
    return E_INVALIDARG;

  *pdwFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
              TF_CLUIE_STRING | TF_CLUIE_CURRENTPAGE;
  return S_OK;
}

STDMETHODIMP CCandidateList::GetDocumentMgr(ITfDocumentMgr** ppdim) {
  *ppdim = nullptr;
  auto pThreadMgr = _tsf->_GetThreadMgr();
  if (pThreadMgr == nullptr) {
    return E_FAIL;
  }
  if (FAILED(pThreadMgr->GetFocus(ppdim)) || (*ppdim == nullptr)) {
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP CCandidateList::GetCount(UINT* pCandidateCount) {
  *pCandidateCount = static_cast<UINT>(_ui->ctx().cinfo.candies.size());
  return S_OK;
}

STDMETHODIMP CCandidateList::GetSelection(UINT* pSelectedCandidateIndex) {
  *pSelectedCandidateIndex = _ui->ctx().cinfo.highlighted;
  return S_OK;
}

STDMETHODIMP CCandidateList::GetString(UINT uIndex, BSTR* pbstr) {
  *pbstr = nullptr;
  auto& cinfo = _ui->ctx().cinfo;
  if (uIndex >= cinfo.candies.size())
    return E_INVALIDARG;

  auto& str = cinfo.candies[uIndex].str;
  *pbstr = SysAllocStringLen(str.c_str(), static_cast<UINT>(str.size()) + 1);

  return S_OK;
}

STDMETHODIMP CCandidateList::GetPageIndex(UINT* pIndex,
                                          UINT uSize,
                                          UINT* puPageCnt) {
  if (!puPageCnt)
    return E_INVALIDARG;
  *puPageCnt = 1;
  if (pIndex) {
    if (uSize < *puPageCnt) {
      return E_INVALIDARG;
    }
    *pIndex = 0;
  }
  return S_OK;
}

STDMETHODIMP CCandidateList::SetPageIndex(UINT* pIndex, UINT uPageCnt) {
  if (!pIndex)
    return E_INVALIDARG;
  return S_OK;
}

STDMETHODIMP CCandidateList::GetCurrentPage(UINT* puPage) {
  *puPage = 0;
  return S_OK;
}

STDMETHODIMP CCandidateList::SetSelection(UINT nIndex) {
  _ui->ctx().cinfo.highlighted = nIndex;
  return S_OK;
}

STDMETHODIMP CCandidateList::Finalize(void) {
  Destroy();
  return S_OK;
}

STDMETHODIMP CCandidateList::Abort(void) {
  _tsf->_AbortComposition(true);
  Destroy();
  return S_OK;
}

STDMETHODIMP CCandidateList::SetIntegrationStyle(GUID guidIntegrationStyle) {
  return S_OK;
}

STDMETHODIMP CCandidateList::GetSelectionStyle(
    TfIntegratableCandidateListSelectionStyle* ptfSelectionStyle) {
  *ptfSelectionStyle = _selectionStyle;
  return S_OK;
}

STDMETHODIMP CCandidateList::OnKeyDown(WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL* pIsEaten) {
  *pIsEaten = TRUE;
  return S_OK;
}

STDMETHODIMP CCandidateList::ShowCandidateNumbers(BOOL* pIsShow) {
  *pIsShow = TRUE;
  return S_OK;
}

STDMETHODIMP CCandidateList::FinalizeExactCompositionString() {
  _tsf->_AbortComposition(false);
  return E_NOTIMPL;
}

void CCandidateList::UpdateUI(const Context& ctx, const Status& status) {
  if (_ui->style().inline_preedit) {
    _ui->style().client_caps |= weasel::INLINE_PREEDIT_CAPABLE;
  } else {
    _ui->style().client_caps &= ~weasel::INLINE_PREEDIT_CAPABLE;
  }

  /// In UWP, candidate window will only be shown
  /// if it is owned by active view window
  //_UpdateOwner();
  _ui->Update(ctx, status);
  _UpdateUIElement();

  if (status.composing)
    Show(_pbShow);
  else
    Show(FALSE);
}

void CCandidateList::UpdateStyle(const UIStyle& sty) {
  _ui->style() = sty;
}

void CCandidateList::UpdateInputPosition(RECT const& rc) {
  if (!_settingsFollowEnabled) {
    _ui->UpdateInputPosition(rc);
    return;
  }
  ++_followSourceUpdates;
  candidate_motion::OwnerGeometry owner;
  RECT effective = rc;
  bool repeatedSource = false;
  if (_ReadOwnerGeometry(owner) &&
      _ownerAnchor.Capture(rc, owner, effective, repeatedSource)) {
    _effectiveAnchor = effective;
    _haveEffectiveAnchor = true;
    if (repeatedSource)
      ++_followRepeatedSources;
  } else {
    _ownerAnchor.Reset();
    _haveEffectiveAnchor = false;
  }
  _ui->UpdateInputPosition(effective);
  _PublishOwnerFollowDiagnostics();
}

void CCandidateList::Destroy() {
  // EndUI();
  Show(FALSE);
  _DisposeUIWindow();
}

void CCandidateList::DestroyAll() {
  // EndUI();
  Show(FALSE);
  _DisposeUIWindowAll();
}
UIStyle& CCandidateList::style() {
  // return _ui->style();
  return _style;
}

HWND CCandidateList::_GetActiveWnd() {
  com_ptr<ITfDocumentMgr> pDocumentMgr;
  com_ptr<ITfContext> pContext;
  com_ptr<ITfContextView> pContextView;
  com_ptr<ITfThreadMgr> pThreadMgr = _tsf->_GetThreadMgr();

  HWND w = NULL;

  // Reset current context
  _pContextDocument = nullptr;

  if (pThreadMgr != nullptr && SUCCEEDED(pThreadMgr->GetFocus(&pDocumentMgr)) &&
      SUCCEEDED(pDocumentMgr->GetTop(&pContext)) &&
      SUCCEEDED(pContext->GetActiveView(&pContextView))) {
    // Set current context
    _pContextDocument = pContext;
    pContextView->GetWnd(&w);
  }

  if (w == NULL)
    w = ::GetFocus();
  return w;
}

HRESULT CCandidateList::_UpdateUIElement() {
  HRESULT hr = S_OK;

  com_ptr<ITfUIElementMgr> pUIElementMgr;
  com_ptr<ITfThreadMgr> pThreadMgr = _tsf->_GetThreadMgr();
  if (nullptr == pThreadMgr) {
    return S_OK;
  }
  hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void**)&pUIElementMgr);

  if (hr == S_OK) {
    pUIElementMgr->UpdateUIElement(uiid);
  }

  return S_OK;
}

void CCandidateList::StartUI() {
  if (_uiStarted)
    return;
  UpdateAcrylicUiProbe(this, 1, S_OK, _pbShow);

  com_ptr<ITfThreadMgr> pThreadMgr = _tsf->_GetThreadMgr();
  if (!pThreadMgr) {
    return;
  }

  com_ptr<ITfUIElementMgr> pUIElementMgr;
  auto hr = pThreadMgr->QueryInterface(&pUIElementMgr);
  if (FAILED(hr))
    return;

  if (pUIElementMgr == NULL) {
    return;
  }

  if (!_ui->uiCallback())
    _ui->SetUICallBack([this](size_t* const sel, size_t* const hov,
                              bool* const next, bool* const scroll_next) {
      _tsf->HandleUICallback(sel, hov, next, scroll_next);
    });
  const HRESULT beginHr = pUIElementMgr->BeginUIElement(this, &_pbShow, &uiid);
  UpdateAcrylicUiProbe(this, 2, beginHr, _pbShow);
  if (FAILED(beginHr))
    return;
  _uiStarted = true;
  // pUIElementMgr->UpdateUIElement(uiid);
  if (_pbShow) {
    _ui->style() = _style;
    _MakeUIWindow();
  }
}

void CCandidateList::EndUI() {
  if (!_uiStarted)
    return;

  com_ptr<ITfThreadMgr> pThreadMgr = _tsf->_GetThreadMgr();
  if (pThreadMgr) {
    com_ptr<ITfUIElementMgr> emgr;
    auto hr = pThreadMgr->QueryInterface(&emgr);
    if (FAILED(hr))
      return;
    if (emgr != NULL)
      emgr->EndUIElement(uiid);
  }
  _uiStarted = false;
  UpdateAcrylicUiProbe(this, 3, S_OK, _pbShow);
  _DisposeUIWindow();
}

com_ptr<ITfContext> CCandidateList::GetContextDocument() {
  return _pContextDocument;
}

void CCandidateList::_DisposeUIWindow() {
  _StopOwnerFollow(true);
  if (_ui == nullptr) {
    return;
  }

  _ui->Destroy();
}

void CCandidateList::_DisposeUIWindowAll() {
  _StopOwnerFollow(true);
  if (_ui == nullptr) {
    return;
  }

  // call _ui->Destroy(true) to clean resources
  _ui->Destroy(true);
}

void CCandidateList::_MakeUIWindow() {
  _StopOwnerFollow(true);
  HWND p = _GetActiveWnd();
  _followView = p;
  _ui->Create(p);
}

bool CCandidateList::_IsSettingsHost() const {
  // Limit this first correction to the one reported host. Do not alter the
  // positioning of Word, WXWork, MailClient or other working clients.
  wchar_t path[32768] = {};
  wchar_t windows[32768] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, path, _countof(path));
  const UINT count = ::GetWindowsDirectoryW(windows, _countof(windows));
  if (!length || length >= _countof(path) || !count ||
      count >= _countof(windows))
    return false;
  const std::wstring expected = std::wstring(windows, count) +
                                L"\\ImmersiveControlPanel\\SystemSettings.exe";
  return ::CompareStringOrdinal(
             path, static_cast<int>(length), expected.c_str(),
             static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

bool CCandidateList::_ReadOwnerGeometry(
    candidate_motion::OwnerGeometry& geometry) const {
  if (!_followView || !::IsWindow(_followView) ||
      !::IsWindowVisible(_followView))
    return false;
  const HWND root = ::GetAncestor(_followView, GA_ROOT);
  if (!root || !::IsWindowVisible(root) || ::IsIconic(root))
    return false;
  RECT client = {};
  POINT origin = {};
  if (!::GetClientRect(_followView, &client) ||
      !::ClientToScreen(_followView, &origin))
    return false;
  using GetDpiFn = UINT(WINAPI*)(HWND);
  const auto getDpi = reinterpret_cast<GetDpiFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  const UINT dpi = getDpi ? getDpi(_followView) : 0;
  if (!dpi || client.right <= client.left || client.bottom <= client.top)
    return false;
  geometry.root = root;
  geometry.origin = origin;
  geometry.width = client.right - client.left;
  geometry.height = client.bottom - client.top;
  geometry.dpi = dpi;
  return true;
}

void CCandidateList::_PublishOwnerFollowDiagnostics() {
  if (!_followWindow)
    return;
  const auto publish = [this](const wchar_t* name, ULONG value) {
    ::SetPropW(_followWindow, name,
               reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(value)));
  };
  publish(L"WeaselOwnerFollowPolicy", 1);
  publish(L"WeaselOwnerSourceUpdates", _followSourceUpdates);
  publish(L"WeaselOwnerTranslations", _followTranslations);
  publish(L"WeaselOwnerRepeatedSources", _followRepeatedSources);
  publish(L"WeaselOwnerLayoutInvalidations", _followLayoutInvalidations);
  publish(L"WeaselOwnerReadFailures", _followReadFailures);
}

LRESULT CALLBACK CCandidateList::_OwnerFollowWndProc(HWND hwnd,
                                                     UINT message,
                                                     WPARAM wParam,
                                                     LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  auto self = reinterpret_cast<CCandidateList*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCDESTROY) {
    ::KillTimer(hwnd, kOwnerFollowTimer);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (self && self->_followWindow == hwnd) {
      self->_followWindow = nullptr;
      self->_followTimerActive = false;
    }
  } else if (message == WM_TIMER && wParam == kOwnerFollowTimer) {
    // KillTimer need not remove an already queued WM_TIMER. A dead/stopped
    // window must not resurrect the UI or retain a callback into freed memory.
    if (!self || self->_followWindow != hwnd || !self->_followTimerActive ||
        self->_followTickBusy)
      return 0;
    self->AddRef();
    self->_followTickBusy = true;
    try {
      self->_TickOwnerFollow();
    } catch (...) {
      ++self->_followReadFailures;
      self->_StopOwnerFollow(false);
    }
    self->_followTickBusy = false;
    self->Release();
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void CCandidateList::_StartOwnerFollow() {
  if (!_settingsFollowEnabled || !_uiStarted || !_pbShow || !_followView ||
      !_ui || !_ui->IsShown() || !_ui->status().composing || _followTimerActive)
    return;
  if (!_followWindow) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = &_OwnerFollowWndProc;
    wc.lpszClassName = kOwnerFollowClass;
    if (!::RegisterClassExW(&wc)) {
      WNDCLASSEXW existing = {};
      existing.cbSize = sizeof(existing);
      if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
          !::GetClassInfoExW(g_hInst, kOwnerFollowClass, &existing) ||
          existing.lpfnWndProc != &_OwnerFollowWndProc)
        return;
    }
    _followWindow = ::CreateWindowExW(0, kOwnerFollowClass, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, g_hInst, this);
    if (!_followWindow) {
      ::UnregisterClassW(kOwnerFollowClass, g_hInst);
      return;
    }
  }
  _followTimerActive = ::SetTimer(_followWindow, kOwnerFollowTimer,
                                  kOwnerFollowIntervalMs, nullptr) != 0;
  _PublishOwnerFollowDiagnostics();
  // The initial TSF position can arrive before _MakeUIWindow. Request one
  // fresh anchor if necessary; never poll the text store on every timer tick.
  if (_followTimerActive && !_haveEffectiveAnchor && _pContextDocument)
    _tsf->_UpdateCompositionWindow(_pContextDocument);
}

void CCandidateList::_StopOwnerFollow(bool destroyWindow) {
  _followTimerActive = false;
  if (_followWindow)
    ::KillTimer(_followWindow, kOwnerFollowTimer);
  if (!destroyWindow)
    return;
  _ownerAnchor.Reset();
  _haveEffectiveAnchor = false;
  _followView = nullptr;
  if (_followWindow) {
    ::DestroyWindow(_followWindow);  // WM_NCDESTROY clears the stored pointer.
    // Other candidate instances can share the class. Unregister only succeeds
    // once the last instance has gone; no foreign class or window is changed.
    ::UnregisterClassW(kOwnerFollowClass, g_hInst);
  }
}

void CCandidateList::_TickOwnerFollow() {
  if (!_settingsFollowEnabled || !_uiStarted || !_pbShow || !_ui ||
      !_ui->IsShown() || !_ui->status().composing || !_pContextDocument)
    return;
  candidate_motion::OwnerGeometry owner;
  if (!_ReadOwnerGeometry(owner)) {
    ++_followReadFailures;
    return;
  }
  const HWND foreground = ::GetForegroundWindow();
  if (!foreground || (::GetAncestor(foreground, GA_ROOT) != owner.root &&
                      ::GetAncestor(foreground, GA_ROOTOWNER) != owner.root))
    return;

  RECT effective = {};
  const auto result = _ownerAnchor.Project(owner, effective);
  if (result == candidate_motion::Projection::kNeedsLayout) {
    // Translation is invalid after resize, view/root change or DPI change.
    // Ask TSF for a new read-only layout, once, rather than guessing scaling or
    // moving the candidate to (0,0). No synchronous wait or edit is introduced.
    _ownerAnchor.Reset();
    _haveEffectiveAnchor = false;
    ++_followLayoutInvalidations;
    _PublishOwnerFollowDiagnostics();
    _tsf->_UpdateCompositionWindow(_pContextDocument);
    return;
  }
  if (result != candidate_motion::Projection::kTranslated ||
      (_haveEffectiveAnchor &&
       candidate_motion::OwnerAnchor::SameRect(effective, _effectiveAnchor)))
    return;

  _effectiveAnchor = effective;
  _haveEffectiveAnchor = true;
  ++_followTranslations;
  // Move the real candidate through its normal UI path. This retains edge
  // avoidance and shadow offsets. No renderer moves a foreign candidate HWND.
  _ui->UpdateInputPosition(effective);
  _PublishOwnerFollowDiagnostics();
}

void WeaselTSF::_UpdateUI(const Context& ctx, const Status& status) {
  _cand->UpdateUI(ctx, status);
}

void WeaselTSF::_StartUI() {
  _cand->StartUI();
}

void WeaselTSF::_EndUI() {
  _cand->EndUI();
}

void WeaselTSF::_ShowUI() {
  _cand->Show(TRUE);
}

void WeaselTSF::_HideUI() {
  _cand->Show(FALSE);
}

com_ptr<ITfContext> WeaselTSF::_GetUIContextDocument() {
  return _cand->GetContextDocument();
}

void WeaselTSF::_DeleteCandidateList() {
  _cand->Destroy();
}

void WeaselTSF::_SelectCandidateOnCurrentPage(size_t index) {
  m_client.SelectCandidateOnCurrentPage(index);
  // simulate a VK_SELECT presskey to get data back and DoEditSession
  // the simulated keycode must be the one make TranslateKeycode Non-Zero return
  // fix me: are there any better ways?
  INPUT inputs[2];
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki = {VK_SELECT, 0, 0, 0, 0};
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki = {VK_SELECT, 0, KEYEVENTF_KEYUP, 0, 0};
  ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
}

void WeaselTSF::_HandleMousePageEvent(bool* const nextPage,
                                      bool* const scrollNextPage) {
  // from scrolling event
  if (scrollNextPage) {
    if (_cand->style().paging_on_scroll)
      m_client.ChangePage(!(*scrollNextPage));
    else {
      UINT current_select = 0, cand_count = 0;
      _cand->GetSelection(&current_select);
      _cand->GetCount(&cand_count);
      bool is_reposition = _cand->GetIsReposition();
      int offset = *scrollNextPage ? 1 : -1;
      offset = offset * (is_reposition ? -1 : 1);
      int index = (int)current_select + offset;
      if (index >= 0 && index < (int)cand_count)
        m_client.HighlightCandidateOnCurrentPage((size_t)index);
      else {
        KeyEvent ke{0, 0};
        ke.keycode = (index < 0) ? ibus::Up : ibus::Down;
        m_client.ProcessKeyEvent(ke);
      }
    }
  } else {  // from click event
    m_client.ChangePage(!(*nextPage));
  }
  _UpdateComposition(_pEditSessionContext);
}

void WeaselTSF::_HandleMouseHoverEvent(const size_t index) {
  UINT current_select = 0;
  _cand->GetSelection(&current_select);

  if (index != current_select) {
    m_client.HighlightCandidateOnCurrentPage(index);
    _UpdateComposition(_pEditSessionContext);
  }
}

void WeaselTSF::HandleUICallback(size_t* const sel,
                                 size_t* const hov,
                                 bool* const next,
                                 bool* const scroll_next) {
  if (sel)
    _SelectCandidateOnCurrentPage(*sel);
  else if (hov)
    _HandleMouseHoverEvent(*hov);
  else if (next || scroll_next)
    _HandleMousePageEvent(next, scroll_next);
}
