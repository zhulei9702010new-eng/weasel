#pragma once
#include <WeaselUI.h>
#include "ctffunc.h"
#include "CandidateOwnerAnchor.h"

class WeaselTSF;

class CCandidateList : public ITfIntegratableCandidateListUIElement,
                       public ITfCandidateListUIElementBehavior {
 public:
  CCandidateList(com_ptr<WeaselTSF> pTextService);
  ~CCandidateList();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void** ppvObj);
  STDMETHODIMP_(ULONG) AddRef(void);
  STDMETHODIMP_(ULONG) Release(void);

  // ITfUIElement
  STDMETHODIMP GetDescription(BSTR* pbstr);
  STDMETHODIMP GetGUID(GUID* pguid);
  STDMETHODIMP Show(BOOL showCandidateWindow);
  STDMETHODIMP IsShown(BOOL* pIsShow);

  // ITfCandidateListUIElement
  STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags);
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim);
  STDMETHODIMP GetCount(UINT* pCandidateCount);
  STDMETHODIMP GetSelection(UINT* pSelectedCandidateIndex);
  STDMETHODIMP GetString(UINT uIndex, BSTR* pbstr);
  STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt);
  STDMETHODIMP SetPageIndex(UINT* pIndex, UINT uPageCnt);
  STDMETHODIMP GetCurrentPage(UINT* puPage);

  // ITfCandidateListUIElementBehavior methods
  STDMETHODIMP SetSelection(UINT nIndex);
  STDMETHODIMP Finalize(void);
  STDMETHODIMP Abort(void);

  // ITfIntegratableCandidateListUIElement methods
  STDMETHODIMP SetIntegrationStyle(GUID guidIntegrationStyle);
  STDMETHODIMP GetSelectionStyle(
      _Out_ TfIntegratableCandidateListSelectionStyle* ptfSelectionStyle);
  STDMETHODIMP OnKeyDown(_In_ WPARAM wParam,
                         _In_ LPARAM lParam,
                         _Out_ BOOL* pIsEaten);
  STDMETHODIMP ShowCandidateNumbers(_Out_ BOOL* pIsShow);
  STDMETHODIMP FinalizeExactCompositionString();

  /* Update */
  void UpdateUI(const weasel::Context& ctx, const weasel::Status& status);
  void UpdateStyle(const weasel::UIStyle& sty);
  void UpdateInputPosition(RECT const& rc);
  void Destroy();
  void DestroyAll();
  void StartUI();
  void EndUI();

  com_ptr<ITfContext> GetContextDocument();
  bool GetIsReposition() {
    if (_ui)
      return _ui->GetIsReposition();
    else
      return false;
  }

  weasel::UIStyle& style();

 private:
  // B.2f v2a: only SystemSettings gets the owner-translation fallback.
  bool _IsSettingsHost() const;
  bool _ReadOwnerGeometry(
      weasel::candidate_motion::OwnerGeometry& geometry) const;
  void _StartOwnerFollow();
  void _StopOwnerFollow(bool destroyWindow);
  void _TickOwnerFollow();
  void _PublishOwnerFollowDiagnostics();
  static LRESULT CALLBACK _OwnerFollowWndProc(HWND hwnd,
                                              UINT message,
                                              WPARAM wParam,
                                              LPARAM lParam);

  const bool _settingsFollowEnabled;
  HWND _followView = nullptr;
  HWND _followWindow = nullptr;
  bool _followTimerActive = false;
  bool _followTickBusy = false;
  bool _haveEffectiveAnchor = false;
  RECT _effectiveAnchor = {};
  weasel::candidate_motion::OwnerAnchor _ownerAnchor;
  ULONG _followSourceUpdates = 0;
  ULONG _followTranslations = 0;
  ULONG _followRepeatedSources = 0;
  ULONG _followLayoutInvalidations = 0;
  ULONG _followReadFailures = 0;

  // void _UpdateOwner();
  HWND _GetActiveWnd();
  HRESULT _UpdateUIElement();

  // for CCandidateList::EndUI(), after ending composition ||
  // WeaselTSF::_EndUI()
  void _DisposeUIWindow();
  // for CCandidateList::Destroy(), when inputing app exit
  void _DisposeUIWindowAll();
  void _MakeUIWindow();

  std::unique_ptr<weasel::UI> _ui;
  DWORD _cRef;
  com_ptr<WeaselTSF> _tsf;
  DWORD uiid;
  TfIntegratableCandidateListSelectionStyle _selectionStyle =
      STYLE_ACTIVE_SELECTION;

  BOOL _pbShow;
  bool _uiStarted = false;
  weasel::UIStyle _style;

  com_ptr<ITfContext> _pContextDocument;
};
