#include "pch.h"
#include "Ui/AxHost.h"

HRESULT AxHost::Create(HWND parent, const wchar_t* clsid)
{
    m_lastErr.clear();
    m_owner = parent;
    RECT rc{ 0, 0, 10, 10 };
    // Passing the CLSID as the window name makes AtlAxWin create and site the control.
    HWND h = m_wnd.Create(parent, rc, clsid,
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0U);
    if (!h)
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        m_lastErr = L"CAxWindow::Create failed";
        return FAILED(hr) ? hr : E_FAIL;
    }
    HRESULT hr = m_wnd.QueryControl(&m_disp);
    if (FAILED(hr)) m_lastErr = L"QueryControl(IDispatch) failed";
    return hr;
}

void AxHost::ConvertToPopup()
{
    if (m_popup || !m_wnd.m_hWnd) return;
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VmAxPopupHost";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    // Owned by the WinUI top-level window: stays above it, hides with it when minimized, no taskbar entry.
    m_popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"VmAxPopupHost", L"", WS_POPUP | WS_CLIPCHILDREN,
                              0, 0, 10, 10, m_owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetParent(m_wnd.m_hWnd, m_popup);
    ShowWindow(m_popup, SW_SHOWNOACTIVATE);
}

void AxHost::MoveInOwnerClient(const RECT& r)
{
    if (!m_wnd.m_hWnd) return;
    if (m_popup)
    {
        POINT tl{ r.left, r.top };
        ClientToScreen(m_owner, &tl);
        SetWindowPos(m_popup, HWND_TOP, tl.x, tl.y, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Move({ 0, 0, r.right - r.left, r.bottom - r.top });
    }
    else
    {
        Move(r);
    }
}

void AxHost::Show(bool visible)
{
    HWND h = m_popup ? m_popup : m_wnd.m_hWnd;
    if (h) ShowWindow(h, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void AxHost::Enable(bool enabled)
{
    HWND h = m_popup ? m_popup : m_wnd.m_hWnd;
    if (h) EnableWindow(h, enabled ? TRUE : FALSE);
}

void AxHost::Destroy()
{
    m_disp.Release();
    if (m_wnd.m_hWnd) m_wnd.DestroyWindow();
    if (m_popup) { DestroyWindow(m_popup); m_popup = nullptr; }
}

void AxHost::Move(const RECT& r)
{
    ::SetWindowPos(m_wnd.m_hWnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

HRESULT AxHost::Invoke(DISPID id, VARIANT* result, std::initializer_list<VARIANT> args)
{
    if (!m_disp) return E_POINTER;
    std::vector<VARIANT> rev(args.begin(), args.end());
    std::reverse(rev.begin(), rev.end());            // DISPPARAMS takes arguments right-to-left
    DISPPARAMS dp{ rev.empty() ? nullptr : rev.data(), nullptr, (UINT)rev.size(), 0 };
    EXCEPINFO ei{};
    UINT argErr = 0;
    HRESULT hr = m_disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, result, &ei, &argErr);
    if (FAILED(hr))
    {
        wchar_t buf[64];
        swprintf_s(buf, L"Invoke(0x%X) hr=0x%08X", (unsigned)id, (unsigned)hr);
        m_lastErr = buf;
        if (ei.bstrDescription) { m_lastErr += L" : "; m_lastErr += ei.bstrDescription; }
    }
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    if (ei.bstrHelpFile) SysFreeString(ei.bstrHelpFile);
    return hr;
}

HRESULT AxHost::CallPtr(DISPID id, const void* p)
{
    VARIANT v; VariantInit(&v);
    v.vt = VT_I8; v.llVal = (LONGLONG)(INT_PTR)p;
    return Invoke(id, nullptr, { v });
}

HRESULT AxHost::CallStr(DISPID id, const wchar_t* s)
{
    ATL::CComBSTR bstr(s);
    VARIANT v; VariantInit(&v);
    v.vt = VT_BSTR; v.bstrVal = bstr;   // bstr owns the string until Invoke returns
    return Invoke(id, nullptr, { v });
}

HRESULT AxHost::CallBool(DISPID id, bool b)
{
    VARIANT v; VariantInit(&v);
    v.vt = VT_BOOL; v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return Invoke(id, nullptr, { v });
}

HRESULT AxHost::CallPtrBool(DISPID id, const void* p, bool b)
{
    VARIANT v1; VariantInit(&v1); v1.vt = VT_I8;   v1.llVal   = (LONGLONG)(INT_PTR)p;
    VARIANT v2; VariantInit(&v2); v2.vt = VT_BOOL; v2.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return Invoke(id, nullptr, { v1, v2 });
}

long long AxHost::CallI8(DISPID id, HRESULT* phr)
{
    VARIANT r; VariantInit(&r);
    HRESULT hr = Invoke(id, &r);
    if (phr) *phr = hr;
    long long v = 0;
    if (SUCCEEDED(hr))
    {
        if (r.vt == VT_I8) v = r.llVal;
        else if (r.vt == VT_I4) v = r.lVal;
        else { VARIANT c; VariantInit(&c); if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_I8))) v = c.llVal; }
    }
    VariantClear(&r);
    return v;
}
