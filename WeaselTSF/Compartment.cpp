#include "stdafx.h"
#include "WeaselTSF.h"
#include "Compartment.h"
#include <resource.h>
#include <functional>
#include <KeyEvent.h>
#include "ResponseParser.h"
#include "CandidateList.h"
#include "LanguageBar.h"

STDAPI CCompartmentEventSink::QueryInterface(REFIID riid,
                                             _Outptr_ void** ppvObj) {
  if (ppvObj == nullptr)
    return E_INVALIDARG;

  *ppvObj = nullptr;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
    *ppvObj = (CCompartmentEventSink*)this;
  }

  if (*ppvObj) {
    AddRef();
    return S_OK;
  }

  return E_NOINTERFACE;
}

STDAPI_(ULONG) CCompartmentEventSink::AddRef() {
  return ++_refCount;
}

STDAPI_(ULONG) CCompartmentEventSink::Release() {
  LONG cr = --_refCount;

  assert(_refCount >= 0);

  if (_refCount == 0) {
    delete this;
  }

  return cr;
}

STDAPI CCompartmentEventSink::OnChange(_In_ REFGUID guidCompartment) {
  return _callback(guidCompartment);
}

// 对应合成 keycode 的 modifier mask（press 事件使用）。
static UINT32 press_mask_for(UINT32 keycode) {
  switch (keycode) {
    case ibus::Shift_L:
    case ibus::Shift_R:
      return ibus::SHIFT_MASK;
    case ibus::Alt_L:
    case ibus::Alt_R:
      return ibus::MOD1_MASK;
    case ibus::Eisu_toggle:
    default:
      return 0;
  }
}

// 修饰键（Shift/Alt）：ascii_composer 在 release 时触发动作，需要发 press+release。
// Toggle 键（Eisu_toggle）：ascii_composer 在 press 时立即触发，release 会再次触发
// 导致动作执行两次相互抵消；只发 press 即可。
static bool should_send_release(UINT32 keycode) {
  switch (keycode) {
    case ibus::Shift_L:
    case ibus::Shift_R:
    case ibus::Alt_L:
    case ibus::Alt_R:
      return true;
    default:
      return false;
  }
}

HRESULT CCompartmentEventSink::_Advise(_In_ com_ptr<IUnknown> punk,
                                       _In_ REFGUID guidCompartment) {
  HRESULT hr = S_OK;
  ITfCompartmentMgr* pCompartmentMgr = nullptr;
  ITfSource* pSource = nullptr;

  hr = punk->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompartmentMgr);
  if (FAILED(hr)) {
    return hr;
  }

  hr = pCompartmentMgr->GetCompartment(guidCompartment, &_compartment);
  if (SUCCEEDED(hr)) {
    hr = _compartment->QueryInterface(IID_ITfSource, (void**)&pSource);
    if (SUCCEEDED(hr)) {
      hr = pSource->AdviseSink(IID_ITfCompartmentEventSink, this, &_cookie);
      pSource->Release();
    }
  }

  pCompartmentMgr->Release();

  return hr;
}
HRESULT CCompartmentEventSink::_Unadvise() {
  HRESULT hr = S_OK;
  ITfSource* pSource = nullptr;

  hr = _compartment->QueryInterface(IID_ITfSource, (void**)&pSource);
  if (SUCCEEDED(hr)) {
    hr = pSource->UnadviseSink(_cookie);
    pSource->Release();
  }

  _compartment = nullptr;
  _cookie = 0;

  return hr;
}

BOOL WeaselTSF::_IsKeyboardDisabled() {
  ITfCompartmentMgr* pCompMgr = NULL;
  ITfDocumentMgr* pDocMgrFocus = NULL;
  ITfContext* pContext = NULL;
  BOOL fDisabled = FALSE;

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) != S_OK) ||
      (pDocMgrFocus == NULL)) {
    fDisabled = TRUE;
    goto Exit;
  }

  if ((pDocMgrFocus->GetTop(&pContext) != S_OK) || (pContext == NULL)) {
    fDisabled = TRUE;
    goto Exit;
  }

  if (pContext->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompMgr) ==
      S_OK) {
    ITfCompartment* pCompartmentDisabled;
    ITfCompartment* pCompartmentEmptyContext;

    /* Check GUID_COMPARTMENT_KEYBOARD_DISABLED */
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED,
                                 &pCompartmentDisabled) == S_OK) {
      VARIANT var;
      if (pCompartmentDisabled->GetValue(&var) == S_OK) {
        if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
          fDisabled = (BOOL)var.lVal;
      }
      pCompartmentDisabled->Release();
    }

    /* Check GUID_COMPARTMENT_EMPTYCONTEXT */
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_EMPTYCONTEXT,
                                 &pCompartmentEmptyContext) == S_OK) {
      VARIANT var;
      if (pCompartmentEmptyContext->GetValue(&var) == S_OK) {
        if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
          fDisabled = (BOOL)var.lVal;
      }
      pCompartmentEmptyContext->Release();
    }
    pCompMgr->Release();
  }

Exit:
  if (pContext)
    pContext->Release();
  if (pDocMgrFocus)
    pDocMgrFocus->Release();
  return fDisabled;
}

BOOL WeaselTSF::_IsKeyboardOpen() {
  com_ptr<ITfCompartmentMgr> pCompMgr;
  BOOL fOpen = FALSE;

  if (_pThreadMgr->QueryInterface(&pCompMgr) == S_OK) {
    com_ptr<ITfCompartment> pCompartment;
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                 &pCompartment) == S_OK) {
      VARIANT var;
      if (pCompartment->GetValue(&var) == S_OK) {
        if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
          fOpen = (BOOL)var.lVal;
      }
    }
  }
  return fOpen;
}

HRESULT WeaselTSF::_SetKeyboardOpen(BOOL fOpen) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfCompartmentMgr> pCompMgr;

  if (_pThreadMgr->QueryInterface(&pCompMgr) == S_OK) {
    ITfCompartment* pCompartment;
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                 &pCompartment) == S_OK) {
      VARIANT var;
      var.vt = VT_I4;
      var.lVal = fOpen;
      hr = pCompartment->SetValue(_tfClientId, &var);
    }
  }

  return hr;
}

HRESULT WeaselTSF::_GetCompartmentDWORD(DWORD& value, const GUID guid) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfCompartmentMgr> pComMgr;
  if (_pThreadMgr->QueryInterface(&pComMgr) == S_OK) {
    ITfCompartment* pCompartment;
    if (pComMgr->GetCompartment(guid, &pCompartment) == S_OK) {
      VARIANT var;
      if (pCompartment->GetValue(&var) == S_OK) {
        if (var.vt == VT_I4)
          value = var.lVal;
        else
          hr = S_FALSE;
      }
    }
    pCompartment->Release();
  }
  return hr;
}

HRESULT WeaselTSF::_SetCompartmentDWORD(const DWORD& value, const GUID guid) {
  HRESULT hr = S_OK;
  com_ptr<ITfCompartmentMgr> pComMgr;
  if (_pThreadMgr->QueryInterface(&pComMgr) == S_OK) {
    ITfCompartment* pCompartment;
    if (pComMgr->GetCompartment(guid, &pCompartment) == S_OK) {
      VARIANT var;
      var.vt = VT_I4;
      var.lVal = value;
      hr = pCompartment->SetValue(_tfClientId, &var);
    }
    pCompartment->Release();
  }
  return hr;
}

BOOL WeaselTSF::_InitCompartment() {
  using namespace std::placeholders;

  auto callback = std::bind(&WeaselTSF::_HandleCompartment, this, _1);
  _pKeyboardCompartmentSink = new CCompartmentEventSink(callback);
  if (!_pKeyboardCompartmentSink)
    return FALSE;
  DWORD hr = _pKeyboardCompartmentSink->_Advise(
      (IUnknown*)_pThreadMgr, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);

  _pConvertionCompartmentSink = new CCompartmentEventSink(callback);
  if (!_pConvertionCompartmentSink)
    return FALSE;
  hr = _pConvertionCompartmentSink->_Advise(
      (IUnknown*)_pThreadMgr, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
  return SUCCEEDED(hr);
}

void WeaselTSF::_UninitCompartment() {
  if (_pKeyboardCompartmentSink) {
    _pKeyboardCompartmentSink->_Unadvise();
    _pKeyboardCompartmentSink = NULL;
  }
  if (_pConvertionCompartmentSink) {
    _pConvertionCompartmentSink->_Unadvise();
    _pConvertionCompartmentSink = NULL;
  }
}

HRESULT WeaselTSF::_HandleCompartment(REFGUID guidCompartment) {
  if (IsEqualGUID(guidCompartment, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
    if (_isToOpenClose) {
      BOOL isOpen = _IsKeyboardOpen();
      // clear composition when close keyboard
      if (!isOpen && _pEditSessionContext) {
        m_client.ClearComposition();
        _EndComposition(_pEditSessionContext, true);
      }
      _EnableLanguageBar(isOpen);
      _UpdateLanguageBar(_status);
    } else {
      // Restore keyboard open first so the server won't drop subsequent
      // key events as "keyboard closed".
      _SetKeyboardOpen(true);

      // Ctrl+Space bypasses the normal key pipeline, so configured
      // key_binder/ascii_composer bindings never see it. Instead synthesize
      // a configurable keycode (default Shift_L) press/release here and let
      // librime's ascii_composer handle the switch according to its
      // switch_key/<keycode> setting (commit_code by default).
      //
      // The synthesized keycode comes from weasel.yaml::ctrl_space_key via
      // server-side Config. 0 means explicitly disabled.
      //
      // Pre-release both Ctrl keys first: the user is physically holding
      // Ctrl while pressing Space, so Ctrl_L is in librime's pending
      // switch_key set. Without clearing it, the synthesized release
      // wouldn't be "pure" and ascii_composer would ignore it.
      UINT32 keycode = _config.ctrl_space_keycode;
      bool config_synced = (keycode != 0);
      if (!config_synced && _EnsureServerConnected()) {
        // On a cold start, TSF may see the default 0 before the first
        // response sync arrives. Pull one response here so we can
        // distinguish "not synced yet" from an explicit disable.
        m_client.ProcessKeyEvent(0);
        weasel::ResponseParser parser(NULL, NULL, &_status, &_config, NULL);
        if (m_client.GetResponseData(std::ref(parser))) {
          keycode = _config.ctrl_space_keycode;
          config_synced = true;
        }
      }
      if (!config_synced) {
        keycode = ibus::Shift_L;
      }
      if (keycode != 0 && _pEditSessionContext && _EnsureServerConnected()) {
        m_client.ProcessKeyEvent(
            weasel::KeyEvent{ibus::Control_L, ibus::RELEASE_MASK});
        m_client.ProcessKeyEvent(
            weasel::KeyEvent{ibus::Control_R, ibus::RELEASE_MASK});

        // Match ConvertKeyEvent's output for a real key press/release:
        //   press  -> corresponding modifier mask (or 0 for non-modifier)
        //   release-> RELEASE_MASK only
        UINT32 pmask = press_mask_for(keycode);
        m_client.ProcessKeyEvent(weasel::KeyEvent{keycode, pmask});
        if (should_send_release(keycode)) {
          m_client.ProcessKeyEvent(
              weasel::KeyEvent{keycode, ibus::RELEASE_MASK});
        }

        _UpdateComposition(_pEditSessionContext);
      } else if (keycode == 0) {
        // Explicitly disabled via ctrl_space_key: disable/none. Keep the
        // keyboard open and refresh the language bar, but don't toggle
        // ascii mode.
      } else {
        // No active edit session context: just toggle ascii_mode locally
        // and tell the server via the tray command path.
        _status.ascii_mode = !_status.ascii_mode;
        _HandleLangBarMenuSelect(_status.ascii_mode
                                     ? ID_WEASELTRAY_ENABLE_ASCII
                                     : ID_WEASELTRAY_DISABLE_ASCII);
        _UpdateLanguageBar(_status);
      }
      if (_pLangBarButton && _pLangBarButton->IsLangBarDisabled())
        _EnableLanguageBar(true);
    }
  } else if (IsEqualGUID(guidCompartment,
                         GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
    BOOL isOpen = _IsKeyboardOpen();
    if (isOpen) {
      weasel::ResponseParser parser(NULL, NULL, &_status, &_config,
                                    &_cand->style());
      bool ok = m_client.GetResponseData(std::ref(parser));
      _UpdateLanguageBar(_status);
    }
  }
  return S_OK;
}
