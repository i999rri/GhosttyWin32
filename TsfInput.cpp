#include <windows.h>
#include "TsfInput.h"
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#ifdef _DEBUG
#define DBG_LOG(msg) OutputDebugStringA(msg)
#else
#define DBG_LOG(msg) ((void)0)
#endif

// --- Edit Session proxy ---
// TSF requires all context reads/writes go through ITfEditSession.
class EditSessionProxy : public ITfEditSession {
public:
    EditSessionProxy(std::function<void(TfEditCookie)> fn) : m_fn(std::move(fn)), m_refCount(1) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override {
        m_fn(ec);
        return S_OK;
    }
private:
    std::function<void(TfEditCookie)> m_fn;
    ULONG m_refCount;
};

// --- TsfInput ---

TsfInput::~TsfInput() {
    Uninitialize();
}

bool TsfInput::Initialize(Callbacks callbacks) {
    m_callbacks = std::move(callbacks);

    // Create thread manager
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgrEx, (void**)&m_threadMgr);
    if (FAILED(hr) || !m_threadMgr) {
        DBG_LOG("TSF: CoCreateInstance TF_ThreadMgr failed\n");
        return false;
    }

    // Activate TSF thread manager with console flag
    // TF_TMAE_CONSOLE enables CUAS (IMM32 emulation) which is needed for IME switching
    hr = m_threadMgr->ActivateEx(&m_clientId, TF_TMAE_CONSOLE);
    if (FAILED(hr)) {
        DBG_LOG("TSF: ActivateEx failed\n");
        return false;
    }

    // Create document manager
    hr = m_threadMgr->CreateDocumentMgr(&m_documentMgr);
    if (FAILED(hr) || !m_documentMgr) {
        DBG_LOG("TSF: CreateDocumentMgr failed\n");
        return false;
    }

    // Create context with this as ITfContextOwnerCompositionSink
    hr = m_documentMgr->CreateContext(m_clientId, 0,
        static_cast<ITfContextOwnerCompositionSink*>(this),
        &m_context, nullptr);
    if (FAILED(hr) || !m_context) {
        DBG_LOG("TSF: CreateContext failed\n");
        return false;
    }

    // Advise sinks on the context's ITfSource
    ComPtr<ITfSource> source;
    hr = m_context->QueryInterface(IID_ITfSource, (void**)source.GetAddressOf());
    if (SUCCEEDED(hr) && source) {
        source->AdviseSink(IID_ITfContextOwner,
            static_cast<ITfContextOwner*>(this), &m_contextOwnerCookie);
        source->AdviseSink(IID_ITfTextEditSink,
            static_cast<ITfTextEditSink*>(this), &m_textEditSinkCookie);
    }

    // Push context onto document manager
    hr = m_documentMgr->Push(m_context);
    if (FAILED(hr)) {
        DBG_LOG("TSF: Push context failed\n");
        return false;
    }

    // Associate focus with the window so IME activation works
    HWND hwnd = m_callbacks.getHwnd ? m_callbacks.getHwnd() : nullptr;
    if (hwnd) {
        ITfDocumentMgr* prevDocMgr = nullptr;
        m_threadMgr->AssociateFocus(hwnd, m_documentMgr, &prevDocMgr);
        if (prevDocMgr) prevDocMgr->Release();
    }

    DBG_LOG("TSF: Initialized successfully\n");
    return true;
}

void TsfInput::Uninitialize() {
    if (m_context) {
        ComPtr<ITfSource> source;
        if (SUCCEEDED(m_context->QueryInterface(IID_ITfSource, (void**)source.GetAddressOf())) && source) {
            if (m_contextOwnerCookie != TF_INVALID_COOKIE)
                source->UnadviseSink(m_contextOwnerCookie);
            if (m_textEditSinkCookie != TF_INVALID_COOKIE)
                source->UnadviseSink(m_textEditSinkCookie);
        }
        m_contextOwnerCookie = TF_INVALID_COOKIE;
        m_textEditSinkCookie = TF_INVALID_COOKIE;
    }

    if (m_documentMgr) {
        m_documentMgr->Pop(TF_POPF_ALL);
        m_documentMgr->Release();
        m_documentMgr = nullptr;
    }
    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }
    if (m_threadMgr) {
        m_threadMgr->Deactivate();
        m_threadMgr->Release();
        m_threadMgr = nullptr;
    }
}

void TsfInput::Focus() {
    if (m_threadMgr && m_documentMgr) {
        m_threadMgr->SetFocus(m_documentMgr);
    }
}

void TsfInput::Unfocus() {
    // Clear composition preview
    if (m_callbacks.handleComposition)
        m_callbacks.handleComposition(L"");
    m_compositionCount = 0;
}

// --- IUnknown ---

ULONG TsfInput::AddRef() { return ++m_refCount; }
ULONG TsfInput::Release() {
    ULONG r = --m_refCount;
    if (r == 0) delete this;
    return r;
}

HRESULT TsfInput::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown) {
        *ppvObj = static_cast<ITfContextOwner*>(this);
    } else if (riid == IID_ITfContextOwner) {
        *ppvObj = static_cast<ITfContextOwner*>(this);
    } else if (riid == IID_ITfContextOwnerCompositionSink) {
        *ppvObj = static_cast<ITfContextOwnerCompositionSink*>(this);
    } else if (riid == IID_ITfTextEditSink) {
        *ppvObj = static_cast<ITfTextEditSink*>(this);
    } else {
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

// --- ITfContextOwner ---

HRESULT TsfInput::GetACPFromPoint(const POINT*, DWORD, LONG*) {
    return E_NOTIMPL;
}

HRESULT TsfInput::GetTextExt(LONG, LONG, RECT* prc, BOOL* pfClipped) {
    if (!prc || !pfClipped) return E_INVALIDARG;
    *prc = m_callbacks.getCursorRect ? m_callbacks.getCursorRect() : RECT{};
    *pfClipped = FALSE;
    return S_OK;
}

HRESULT TsfInput::GetScreenExt(RECT* prc) {
    if (!prc) return E_INVALIDARG;
    *prc = m_callbacks.getViewportRect ? m_callbacks.getViewportRect() : RECT{};
    return S_OK;
}

HRESULT TsfInput::GetStatus(TF_STATUS* pdcs) {
    if (!pdcs) return E_INVALIDARG;
    // TF_SS_TRANSITORY: text is sent immediately, no undo history
    pdcs->dwDynamicFlags = 0;
    pdcs->dwStaticFlags = TF_SS_TRANSITORY | TS_SS_NOHIDDENTEXT;
    return S_OK;
}

HRESULT TsfInput::GetWnd(HWND* phwnd) {
    if (!phwnd) return E_INVALIDARG;
    *phwnd = m_callbacks.getHwnd ? m_callbacks.getHwnd() : nullptr;
    return S_OK;
}

HRESULT TsfInput::GetAttribute(REFGUID, VARIANT* pvarValue) {
    if (!pvarValue) return E_INVALIDARG;
    pvarValue->vt = VT_EMPTY;
    return S_OK;
}

// --- ITfContextOwnerCompositionSink ---

HRESULT TsfInput::OnStartComposition(ITfCompositionView*, BOOL* pfOk) {
    if (pfOk) *pfOk = TRUE;
    m_compositionCount++;
    DBG_LOG("TSF: OnStartComposition\n");
    return S_OK;
}

HRESULT TsfInput::OnUpdateComposition(ITfCompositionView*, ITfRange*) {
    return S_OK;
}

HRESULT TsfInput::OnEndComposition(ITfCompositionView*) {
    m_compositionCount--;
    DBG_LOG("TSF: OnEndComposition\n");

    // Flush remaining text
    if (m_compositionCount <= 0 && m_context) {
        m_compositionCount = 0;
        // Request edit session to read final text
        auto* session = new EditSessionProxy([this](TfEditCookie ec) {
            ReadCompositionText(ec);
        });
        HRESULT hrSession = S_OK;
        m_context->RequestEditSession(m_clientId, session,
            TF_ES_READWRITE | TF_ES_ASYNC, &hrSession);
        session->Release();

        // Clear composition preview
        if (m_callbacks.handleComposition)
            m_callbacks.handleComposition(L"");
    }
    return S_OK;
}

// --- ITfTextEditSink ---

HRESULT TsfInput::OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord*) {
    if (!m_context || pic != m_context) return S_OK;

    // Request a read-write edit session to process text
    auto* session = new EditSessionProxy([this](TfEditCookie ec) {
        ReadCompositionText(ec);
    });
    HRESULT hrSession = S_OK;
    m_context->RequestEditSession(m_clientId, session,
        TF_ES_READWRITE | TF_ES_ASYNC, &hrSession);
    session->Release();

    return S_OK;
}

// --- Internal ---

void TsfInput::ReadCompositionText(TfEditCookie ec) {
    if (!m_context) return;

    // Get the full text range
    ComPtr<ITfRange> fullRange;
    {
        ComPtr<ITfRange> start;
        ComPtr<ITfRange> end;
        m_context->GetStart(ec, start.GetAddressOf());
        m_context->GetEnd(ec, end.GetAddressOf());
        if (!start || !end) return;
        start->Clone(fullRange.GetAddressOf());
        if (!fullRange) return;
        fullRange->ShiftEndToRange(ec, end.Get(), TF_ANCHOR_END);
    }

    // Read text from the range
    wchar_t buf[1024] = {};
    ULONG fetched = 0;
    fullRange->GetText(ec, 0, buf, 1023, &fetched);
    if (fetched == 0) return;

    std::wstring text(buf, fetched);

    if (m_compositionCount > 0) {
        // Still composing — show preview
        if (m_callbacks.handleComposition)
            m_callbacks.handleComposition(text);
    } else {
        // Composition done — send finalized text and clear the context
        if (m_callbacks.handleOutput)
            m_callbacks.handleOutput(text);

        // Clear the context text (transitory document)
        ComPtr<ITfRange> clearRange;
        m_context->GetStart(ec, clearRange.GetAddressOf());
        if (clearRange) {
            clearRange->ShiftEndToRange(ec, fullRange.Get(), TF_ANCHOR_END);
            clearRange->SetText(ec, 0, nullptr, 0);
        }
    }
}
