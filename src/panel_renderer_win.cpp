// Windows implementation of PanelRenderer: subclasses AE's container
// HWND (the AEGP_PlatformViewRef), sets up a DX11 swap chain on it,
// and drives a Dear ImGui frame on every WM_PAINT plus a ~60fps timer.
//
// We follow the SDK Panelator pattern of subclassing the container
// rather than creating a child HWND — simpler, no resize forwarding
// needed, and AE's own paint stays out of the way once we stop
// chaining WM_PAINT to the old proc.

#include "panel_renderer.h"
#include "panel_state.h"
#include "panel_ui.h"
#include "file_dialog.h"
#include "exr_scan.h"
#include "session_io.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <ole2.h>          // OleInitialize / RegisterDragDrop / IDropTarget
#include <shellapi.h>      // DragQueryFile* / CF_HDROP
#include <shlobj.h>

#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ClearActiveID
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ImGui's Win32 backend exports a wndproc helper that consumes the
// input events (mouse, keys, etc.) it cares about.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr UINT_PTR kRedrawTimerId = 0xC4A5E;     // arbitrary
constexpr UINT     kRedrawIntervalMs = 16;       // ~60 Hz
constexpr char     kHostHwndProp[] = "ChaseMakerPanelRenderer";
constexpr UINT     kMsgGrabFocus = WM_USER + 1;

// Forward declaration so the drop target can call back into the
// renderer to start scans.
class WinPanelRenderer;

// IDropTarget impl. Holds a raw pointer to the renderer; the
// renderer's destructor calls DetachOwner before releasing us.
class CMDropTarget : public IDropTarget
{
public:
    explicit CMDropTarget(WinPanelRenderer* owner) : i_owner(owner), i_ref(1) {}
    void DetachOwner() { i_owner = nullptr; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++i_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --i_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL,
                                        DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD*) override;

private:
    WinPanelRenderer* i_owner;
    ULONG             i_ref;
};

class WinPanelRenderer : public PanelRenderer
{
public:
    WinPanelRenderer(HWND host, PanelState* state)
        : i_host(host), i_state(state)
    {
        if (!CreateDeviceAndSwapChain()) {
            Cleanup();
            return;
        }
        InitImGui();
        Subclass();

        // Drag-and-drop registration. OleInitialize is per-thread,
        // ref-counted; safe to call even if AE already initialized it.
        if (SUCCEEDED(OleInitialize(nullptr))) {
            i_ole_initialized = true;
        }
        i_drop_target = new CMDropTarget(this);
        // RegisterDragDrop AddRefs the target; we keep our own ref so
        // we can DetachOwner before AE-side teardown.
        if (RegisterDragDrop(i_host, i_drop_target) != S_OK) {
            i_drop_target->Release();
            i_drop_target = nullptr;
        }

        // Drive ~60 Hz redraws so ImGui animations (cursor blink,
        // hover transitions) run smoothly. If this proves too chatty
        // later we can switch to redraw-on-demand only.
        SetTimer(i_host, kRedrawTimerId, kRedrawIntervalMs, nullptr);

        i_ready = true;
    }

    ~WinPanelRenderer() override
    {
        KillTimer(i_host, kRedrawTimerId);
        if (i_drop_target) {
            RevokeDragDrop(i_host);
            i_drop_target->DetachOwner();
            i_drop_target->Release();
            i_drop_target = nullptr;
        }
        if (i_ole_initialized) {
            OleUninitialize();
            i_ole_initialized = false;
        }
        Unsubclass();
        ReleaseAllThumbnailTextures();
        ShutdownImGui();
        Cleanup();
    }

    // Called by CMDropTarget after a successful drop with one or more
    // file paths. Each path becomes a new source via the existing scan.
    void OnDroppedFiles(const std::vector<std::string>& paths)
    {
        for (const auto& p : paths) {
            if (p.empty()) continue;
            exr_scan::StartScan(p, i_state, /*append=*/true, /*source_id=*/0);
        }
        // Status update so user sees feedback even if scan is queued.
        if (!paths.empty() && i_state) {
            std::lock_guard<std::mutex> lk(i_state->mu);
            i_state->last_status = "Dropped " + std::to_string(paths.size()) +
                                   " file(s) — scanning...";
            i_state->last_error.clear();
        }
        InvalidateRect(i_host, nullptr, FALSE);
    }

    // Called by CMDropTarget when a drop arrived with no file-path
    // format. Logs the available format names so we can identify
    // AE's drag format and add support for it.
    void OnUnknownDrop(IDataObject* obj)
    {
        if (!obj || !i_state) return;
        std::string msg = "Drop received but no file path. Formats:";
        IEnumFORMATETC* enumFmt = nullptr;
        if (SUCCEEDED(obj->EnumFormatEtc(DATADIR_GET, &enumFmt)) && enumFmt) {
            FORMATETC fmt;
            int count = 0;
            while (enumFmt->Next(1, &fmt, nullptr) == S_OK && count < 32) {
                char name[256] = {};
                if (fmt.cfFormat >= 0xC000) {
                    GetClipboardFormatNameA(fmt.cfFormat, name, sizeof(name) - 1);
                } else {
                    std::snprintf(name, sizeof(name), "CF_%u", fmt.cfFormat);
                }
                msg += " ["; msg += name; msg += "]";
                if (fmt.ptd) CoTaskMemFree(fmt.ptd);
                ++count;
            }
            enumFmt->Release();
        }
        std::lock_guard<std::mutex> lk(i_state->mu);
        i_state->last_status = msg;
        InvalidateRect(i_host, nullptr, FALSE);
    }

    LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        // Handle our deferred-focus message. Fires AFTER the message
        // that triggered the focus request has fully returned, so
        // AE's WM_KILLFOCUS handler doesn't run nested inside our
        // mouse-down handler — that nesting was the AE hang.
        if (msg == kMsgGrabFocus) {
            if (GetFocus() != i_host) SetFocus(i_host);
            return 0;
        }

        // Grab OS-level keyboard focus on mouse-down inside our
        // panel — deferred via PostMessage so the focus change
        // runs OUTSIDE the current message handler (synchronous
        // SetFocus from inside the click handler hung AE earlier).
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
            msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN) {
            if (GetFocus() != i_host) {
                PostMessage(i_host, kMsgGrabFocus, 0, 0);
            }
        }

        if (i_imguiCtx) {
            ImGui::SetCurrentContext(i_imguiCtx);
            if (ImGui_ImplWin32_WndProcHandler(i_host, msg, wparam, lparam)) {
                return 0;
            }

            // ImGui's WndProcHandler updates IO state but doesn't
            // consume keyboard messages by default — meaning the
            // keystroke also falls through to AE and is processed
            // as a hotkey. Block that pass-through when ImGui
            // actually wants the key (text input or any active
            // keyboard capture), AND for our panel-global
            // shortcuts (Spacebar, Ctrl+Z, Ctrl+Y) when our HWND
            // owns OS focus — otherwise AE's spacebar starts comp
            // preview instead of our staging/chase preview.
            const bool is_keyboard =
                msg == WM_KEYDOWN  || msg == WM_KEYUP    ||
                msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                msg == WM_CHAR     || msg == WM_DEADCHAR ||
                msg == WM_SYSCHAR  || msg == WM_UNICHAR;
            if (is_keyboard) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.WantCaptureKeyboard || io.WantTextInput) {
                    return 0;
                }
                if (GetFocus() == i_host &&
                    (msg == WM_KEYDOWN || msg == WM_KEYUP ||
                     msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                     msg == WM_CHAR))
                {
                    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (wparam == VK_SPACE) return 0;
                    if (ctrl && (wparam == 'Z' || wparam == 'Y')) return 0;
                }
            }

            // When we lose OS focus (user clicked outside the panel),
            // clear ImGui's active widget so the previously-active
            // InputText releases — otherwise WantTextInput would stay
            // 1 forever and we'd keep eating keystrokes.
            if (msg == WM_KILLFOCUS) {
                ImGui::ClearActiveID();
            }
        }

        switch (msg) {
        case WM_PAINT: {
            // Skip our own DX11 paint while a modal file dialog is
            // pumping messages — the OS still delivers WM_PAINT to
            // expose-uncovered regions and we'd reenter ImGui.
            if (!i_in_dialog) {
                RenderFrame();
            }
            ValidateRect(i_host, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (i_device && wparam != SIZE_MINIMIZED) {
                ReleaseRenderTarget();
                HRESULT hr = i_swapChain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr)) {
                    CreateRenderTarget();
                }
            }
            return 0;
        }
        case WM_TIMER: {
            if (wparam == kRedrawTimerId) {
                InvalidateRect(i_host, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1; // DX11 paints the full client area; skip GDI erase.
        }

        if (i_prevWndProc) {
            return CallWindowProc(i_prevWndProc, i_host, msg, wparam, lparam);
        }
        return DefWindowProc(i_host, msg, wparam, lparam);
    }

private:
    bool CreateDeviceAndSwapChain()
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = i_host;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL gotLevel;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &desc, &i_swapChain, &i_device, &gotLevel, &i_context);
        if (FAILED(hr)) {
            return false;
        }
        CreateRenderTarget();
        return true;
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(i_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
            i_device->CreateRenderTargetView(backBuffer, nullptr, &i_rtv);
            backBuffer->Release();
        }
    }

    void ReleaseRenderTarget()
    {
        if (i_rtv) { i_rtv->Release(); i_rtv = nullptr; }
    }

    void Cleanup()
    {
        ReleaseRenderTarget();
        if (i_swapChain) { i_swapChain->Release(); i_swapChain = nullptr; }
        if (i_context)   { i_context->Release();   i_context = nullptr; }
        if (i_device)    { i_device->Release();    i_device = nullptr; }
    }

    void InitImGui()
    {
        IMGUI_CHECKVERSION();
        i_imguiCtx = ImGui::CreateContext();
        ImGui::SetCurrentContext(i_imguiCtx);

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // don't persist layout to disk
        io.LogFilename = nullptr;

        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(i_host);
        ImGui_ImplDX11_Init(i_device, i_context);
    }

    void ShutdownImGui()
    {
        if (!i_imguiCtx) return;
        ImGui::SetCurrentContext(i_imguiCtx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(i_imguiCtx);
        i_imguiCtx = nullptr;
    }

    void RenderFrame()
    {
        if (!i_imguiCtx || !i_rtv) return;
        if (i_frame_in_progress) return;   // skip reentrant calls
        i_frame_in_progress = true;
        ImGui::SetCurrentContext(i_imguiCtx);

        EnsureThumbnailTextures();
        UpdateChasePreviewTexture();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT rc;
        GetClientRect(i_host, &rc);
        panel_ui::RenderFrame(i_state,
            static_cast<float>(rc.right - rc.left),
            static_cast<float>(rc.bottom - rc.top),
            i_host,
            "Windows / DX11");

        ImGui::Render();

        const float clearColor[4] = {0.10f, 0.10f, 0.11f, 1.0f};
        i_context->OMSetRenderTargets(1, &i_rtv, nullptr);
        i_context->ClearRenderTargetView(i_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        i_swapChain->Present(1, 0);

        i_frame_in_progress = false;

        // Drain any deferred actions the UI requested this frame.
        // These spin nested message loops (file dialog) or do disk
        // I/O (sidecar) — must not run while ImGui is mid-frame.
        HandleDeferredActions();
    }

    void ReleaseAllThumbnailTextures()
    {
        for (auto* srv : i_thumb_srvs) {
            if (srv) srv->Release();
        }
        i_thumb_srvs.clear();
        i_last_scan_gen = -1;
        if (i_chase_composite_srv) {
            i_chase_composite_srv->Release();
            i_chase_composite_srv = nullptr;
        }
        if (i_state) i_state->chase_composite_texture_id = 0;
    }

    void UpdateChasePreviewTexture()
    {
        if (!i_state || !i_device) return;
        if (!i_state->chase_composite_dirty.exchange(false)) return;
        if (i_chase_composite_srv) {
            i_chase_composite_srv->Release();
            i_chase_composite_srv = nullptr;
        }
        const int w = i_state->chase_composite_w;
        const int h = i_state->chase_composite_h;
        if (w <= 0 || h <= 0 || i_state->chase_composite_rgba.empty()) {
            i_state->chase_composite_texture_id = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = i_state->chase_composite_rgba.data();
        srd.SysMemPitch = w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(i_device->CreateTexture2D(&desc, &srd, &tex)) || !tex) {
            i_state->chase_composite_texture_id = 0;
            return;
        }
        if (FAILED(i_device->CreateShaderResourceView(tex, nullptr, &i_chase_composite_srv))) {
            tex->Release();
            i_state->chase_composite_texture_id = 0;
            return;
        }
        tex->Release();
        i_state->chase_composite_texture_id =
            reinterpret_cast<uint64_t>(i_chase_composite_srv);
    }

    void EnsureThumbnailTextures()
    {
        if (!i_state || !i_device) return;
        const int gen = i_state->scan_generation.load();
        if (gen != i_last_scan_gen) {
            ReleaseAllThumbnailTextures();
            i_last_scan_gen = gen;
        }

        std::lock_guard<std::mutex> lk(i_state->mu);
        Source* src = ActiveSource(*i_state);
        if (!src) return;
        for (size_t idx = 0; idx < src->layers.size(); ++idx) {
            LayerInfo& L = src->layers[idx];
            if (L.texture_id != 0) continue;
            if (L.thumb_rgba.empty() || L.thumb_w <= 0 || L.thumb_h <= 0) continue;

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = L.thumb_w;
            desc.Height = L.thumb_h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA srd{};
            srd.pSysMem = L.thumb_rgba.data();
            srd.SysMemPitch = L.thumb_w * 4;

            ID3D11Texture2D* tex = nullptr;
            if (FAILED(i_device->CreateTexture2D(&desc, &srd, &tex)) || !tex) {
                continue;
            }
            ID3D11ShaderResourceView* srv = nullptr;
            HRESULT hr = i_device->CreateShaderResourceView(tex, nullptr, &srv);
            tex->Release();
            if (FAILED(hr) || !srv) continue;

            i_thumb_srvs.push_back(srv);
            L.texture_id = reinterpret_cast<uint64_t>(srv);
        }
    }

    void HandleDeferredActions()
    {
        auto run_dialog = [&](auto&& dialog_call) {
            // Kill the redraw timer so WM_TIMER doesn't reentrantly
            // trigger a paint while the dialog's nested message loop
            // is spinning. Restore after.
            KillTimer(i_host, kRedrawTimerId);
            i_in_dialog = true;
            auto result = dialog_call();
            i_in_dialog = false;
            SetTimer(i_host, kRedrawTimerId, kRedrawIntervalMs, nullptr);
            return result;
        };

        if (i_state->want_pick_exr.exchange(false)) {
            std::string path = run_dialog([&](){ return file_dialog::PickExr(i_host); });
            if (!path.empty()) {
                exr_scan::StartScan(path, i_state, /*append=*/true, /*source_id=*/0);
            }
            InvalidateRect(i_host, nullptr, FALSE);
        }
        if (i_state->want_write_sidecar.exchange(false)) {
            exr_scan::WriteLuminositySidecar(i_state);
            InvalidateRect(i_host, nullptr, FALSE);
        }
        if (i_state->want_save_session.exchange(false)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(i_state->mu);
                path = i_state->session_save_path;
            }
            if (path.empty()) {
                path = run_dialog([&](){ return file_dialog::PickSessionSavePath(i_host); });
            }
            if (!path.empty()) {
                session_io::WriteSession(i_state, path);
            }
            InvalidateRect(i_host, nullptr, FALSE);
        }
        if (i_state->want_load_session.exchange(false)) {
            std::string path = run_dialog([&](){
                return file_dialog::PickSessionLoadPath(i_host);
            });
            if (!path.empty()) {
                session_io::LoadSession(i_state, path);
            }
            InvalidateRect(i_host, nullptr, FALSE);
        }
        // want_build_* flags are drained by the AEGP idle hook in
        // chase_maker.cpp — calling ae_build from this render-thread
        // context returns "no project" because AE only exposes the
        // project handle inside registered hooks.
    }

    void Subclass()
    {
        SetPropA(i_host, kHostHwndProp, reinterpret_cast<HANDLE>(this));
        i_prevWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(i_host, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(SWndProc)));
    }

    void Unsubclass()
    {
        if (i_prevWndProc) {
            SetWindowLongPtrA(i_host, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(i_prevWndProc));
            i_prevWndProc = nullptr;
        }
        RemovePropA(i_host, kHostHwndProp);
    }

    static LRESULT CALLBACK SWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<WinPanelRenderer*>(
            GetPropA(hwnd, kHostHwndProp));
        if (self) {
            return self->WndProc(msg, wp, lp);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    HWND                    i_host = nullptr;
    WNDPROC                 i_prevWndProc = nullptr;
    ID3D11Device*           i_device = nullptr;
    ID3D11DeviceContext*    i_context = nullptr;
    IDXGISwapChain*         i_swapChain = nullptr;
    ID3D11RenderTargetView* i_rtv = nullptr;
    ImGuiContext*           i_imguiCtx = nullptr;
    bool                    i_ready = false;
    bool                    i_frame_in_progress = false;
    bool                    i_in_dialog = false;
    PanelState*             i_state = nullptr;
    std::vector<ID3D11ShaderResourceView*> i_thumb_srvs;
    int                     i_last_scan_gen = -1;
    ID3D11ShaderResourceView* i_chase_composite_srv = nullptr;
    CMDropTarget*           i_drop_target = nullptr;
    bool                    i_ole_initialized = false;
};

// Now that WinPanelRenderer is complete, define CMDropTarget::Drop.
HRESULT STDMETHODCALLTYPE CMDropTarget::Drop(IDataObject* pDataObj,
                                              DWORD, POINTL,
                                              DWORD* pdwEffect)
{
    if (pdwEffect) *pdwEffect = DROPEFFECT_NONE;
    if (!pDataObj || !i_owner) return S_OK;

    // Try CF_HDROP first: Explorer-style file drops, and probably what
    // AE's Project panel uses for footage items (since AE's underlying
    // representation of an imported footage is a file path).
    FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    std::vector<std::string> paths;
    if (SUCCEEDED(pDataObj->GetData(&fmt, &medium))) {
        HDROP hdrop = reinterpret_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (hdrop) {
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                if (len == 0) continue;
                std::vector<wchar_t> wbuf(static_cast<size_t>(len) + 1);
                DragQueryFileW(hdrop, i, wbuf.data(),
                               static_cast<UINT>(wbuf.size()));
                int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), -1,
                                                nullptr, 0, nullptr, nullptr);
                if (u8len > 1) {
                    std::string p(static_cast<size_t>(u8len - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), -1,
                                        p.data(), u8len, nullptr, nullptr);
                    paths.push_back(std::move(p));
                }
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    if (!paths.empty()) {
        i_owner->OnDroppedFiles(paths);
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
    } else {
        // No file-path format — log everything so we can identify the
        // AE Project panel's drag format and add explicit support.
        i_owner->OnUnknownDrop(pDataObj);
    }
    return S_OK;
}

} // namespace

PanelRenderer* CreatePanelRenderer(void* container, PanelState* state)
{
    HWND hwnd = static_cast<HWND>(container);
    if (!hwnd || !IsWindow(hwnd) || !state) return nullptr;
    return new WinPanelRenderer(hwnd, state);
}
