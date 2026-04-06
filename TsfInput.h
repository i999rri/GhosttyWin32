#pragma once
#include <windows.h>
#include <msctf.h>
#include <string>
#include <functional>

#pragma comment(lib, "ole32.lib")

// Minimal TSF 1.0 implementation for IME text input.
// Based on Windows Terminal's TSF architecture (PR #17067).

class TsfInput : public ITfContextOwner,
                 public ITfContextOwnerCompositionSink,
                 public ITfTextEditSink
{
public:
    // Callbacks the host must provide
    struct Callbacks {
        std::function<HWND()> getHwnd;
        std::function<RECT()> getCursorRect;    // Screen coordinates
        std::function<RECT()> getViewportRect;  // Screen coordinates
        std::function<void(std::wstring_view)> handleOutput; // Finalized text
        std::function<void(std::wstring_view)> handleComposition; // Preview text (nullable)
    };

    TsfInput() = default;
    ~TsfInput();

    bool Initialize(Callbacks callbacks);
    void Uninitialize();
    void Focus();
    void Unfocus();

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;

    // ITfContextOwner
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(const POINT* ptScreen, DWORD dwFlags, LONG* pacp) override;
    HRESULT STDMETHODCALLTYPE GetTextExt(LONG acpStart, LONG acpEnd, RECT* prc, BOOL* pfClipped) override;
    HRESULT STDMETHODCALLTYPE GetScreenExt(RECT* prc) override;
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS* pdcs) override;
    HRESULT STDMETHODCALLTYPE GetWnd(HWND* phwnd) override;
    HRESULT STDMETHODCALLTYPE GetAttribute(REFGUID rguidAttribute, VARIANT* pvarValue) override;

    // ITfContextOwnerCompositionSink
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView* pComposition, BOOL* pfOk) override;
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView* pComposition, ITfRange* pRangeNew) override;
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView* pComposition) override;

    // ITfTextEditSink
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord) override;

private:
    // Edit session helper
    HRESULT RequestEditSession(TfEditCookie* pec);
    void ReadCompositionText(TfEditCookie ec);

    ULONG m_refCount = 1;
    Callbacks m_callbacks;

    ITfThreadMgrEx* m_threadMgr = nullptr;
    ITfDocumentMgr* m_documentMgr = nullptr;
    ITfContext* m_context = nullptr;
    TfClientId m_clientId = TF_CLIENTID_NULL;
    DWORD m_contextOwnerCookie = TF_INVALID_COOKIE;
    DWORD m_textEditSinkCookie = TF_INVALID_COOKIE;
    int m_compositionCount = 0;
};
