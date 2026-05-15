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
#include "diag_log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>     // IDXGISwapChain2 — frame-latency waitable object
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
constexpr UINT     kRedrawIntervalMs = 33;       // ~30 Hz — halve the
// per-frame GPU + Present pressure we put on AE's main UI thread
// (RenderFrame runs there). 30 Hz is plenty for a tool panel and
// markedly cuts contention with AE's own GPU rendering.
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
        CM_DIAG_LOG("WinRenderer ctor: enter (host=%p)", (void*)host);
        CM_DIAG_LOG("WinRenderer ctor: -> D3D11CreateDeviceAndSwapChain");
        if (!CreateDeviceAndSwapChain()) {
            CM_DIAG_LOG("WinRenderer ctor: D3D init FAILED — bailing");
            Cleanup();
            return;
        }
        CM_DIAG_LOG("WinRenderer ctor: D3D ok -> InitImGui");
        InitImGui();
        CM_DIAG_LOG("WinRenderer ctor: ImGui ok -> Subclass");
        Subclass();
        CM_DIAG_LOG("WinRenderer ctor: subclassed");

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
        CM_DIAG_LOG("WinRenderer ctor: ready (timer armed)");
    }

    ~WinPanelRenderer() override
    {
        CM_DIAG_LOG("dtor: begin (renderer=%p host=%p)",
                    (void*)this, (void*)i_host);
        KillTimer(i_host, kRedrawTimerId);
        CM_DIAG_LOG("dtor: timer killed");
        if (i_drop_target) {
            RevokeDragDrop(i_host);
            i_drop_target->DetachOwner();
            i_drop_target->Release();
            i_drop_target = nullptr;
        }
        CM_DIAG_LOG("dtor: drag-drop revoked");
        if (i_ole_initialized) {
            OleUninitialize();
            i_ole_initialized = false;
        }
        CM_DIAG_LOG("dtor: OLE uninit");
        Unsubclass();
        CM_DIAG_LOG("dtor: unsubclassed");
        ReleaseAllThumbnailTextures();
        CM_DIAG_LOG("dtor: thumbnails released");
        ShutdownImGui();
        CM_DIAG_LOG("dtor: ImGui shutdown");
        Cleanup();
        CM_DIAG_LOG("dtor: done (D3D released)");
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
                // Must keep the WAITABLE flag on resize, else the
                // frame-latency object stops working. The waitable
                // handle itself stays valid across ResizeBuffers.
                HRESULT hr = i_swapChain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN,
                    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
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
        case WM_NCDESTROY: {
            // AE destroyed the panel's container window (panel closed).
            // Nothing tears this renderer down otherwise — there's no
            // AEGP panel-destroy callback and AE makes a brand-new
            // container on reopen. Without this we leak the renderer
            // (D3D device, ImGui ctx, subclass, timer) every close and
            // a zombie WM_TIMER keeps racing the next renderer over
            // the shared PanelState. Canonical Win32 subclass teardown:
            // delete self, then chain to the original wndproc. Capture
            // what we need first — `this` is gone after delete.
            WNDPROC prev = i_prevWndProc;
            HWND h = i_host;
            CM_DIAG_LOG("WM_NCDESTROY: enter (renderer=%p host=%p) "
                        "-> delete this", (void*)this, (void*)h);
            delete this;   // ~WinPanelRenderer: Unsubclass + release all
            CM_DIAG_LOG("WM_NCDESTROY: deleted; chaining prev wndproc");
            if (prev) return CallWindowProc(prev, h, msg, wparam, lparam);
            return DefWindowProc(h, msg, wparam, lparam);
        }
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
        // FLIP model + a frame-latency WAITABLE object. This is the
        // real fix for the AE-wide freeze: three symbolized dumps showed
        // AE's main UI thread parked in dxgi!Present -> nvwgf2umx from
        // our RenderFrame (bitblt DISCARD + sync-interval/DO_NOT_WAIT
        // tweaks could NOT stop it). With the waitable + max-latency 1,
        // we poll the waitable non-blocking at the top of RenderFrame
        // and only ever render/Present when the swapchain says the GPU
        // is ready — so Present never has an outstanding frame to block
        // on, and AE's thread is never parked in the driver.
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

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
        // Use the WARP software rasterizer, NOT the hardware (NVIDIA)
        // driver. EVERY crash/hang dump in this investigation faults
        // or blocks inside nvwgf2umx.dll (the NVIDIA D3D UMD): AE alone
        // runs the project fine forever, but our panel adding a SECOND
        // hardware D3D11 device on the same driver in AE's process
        // destabilises it (UI-thread Present hangs, then a stack
        // overflow inside the driver's own worker thread — confirmed
        // by the AE/Sentry fault dump, 0 ChaseMaker frames on it).
        // Our panel is a trivial 2D ImGui UI — it does not need GPU
        // acceleration. WARP keeps us entirely off nvwgf2umx; AE still
        // owns the GPU for its rendering. If WARP somehow fails, fall
        // back to hardware so the panel at least draws.
        CM_DIAG_LOG("D3D: D3D11CreateDeviceAndSwapChain call (WARP)");
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &desc, &i_swapChain, &i_device, &gotLevel, &i_context);
        CM_DIAG_LOG("D3D: WARP create hr=0x%08lx", (unsigned long)hr);
        if (FAILED(hr)) {
            CM_DIAG_LOG("D3D: WARP failed -> falling back to HARDWARE");
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                &desc, &i_swapChain, &i_device, &gotLevel, &i_context);
            CM_DIAG_LOG("D3D: HARDWARE fallback hr=0x%08lx",
                        (unsigned long)hr);
        }
        if (FAILED(hr)) {
            return false;
        }
        // Grab the frame-latency waitable + cap queued frames at 1.
        {
            IDXGISwapChain2* sc2 = nullptr;
            if (SUCCEEDED(i_swapChain->QueryInterface(IID_PPV_ARGS(&sc2))) &&
                sc2)
            {
                sc2->SetMaximumFrameLatency(1);
                i_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
                sc2->Release();
            }
            CM_DIAG_LOG("D3D: flip-model swapchain, waitable=%p",
                        (void*)i_frameLatencyWaitable);
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
        if (i_frameLatencyWaitable) {
            CloseHandle(i_frameLatencyWaitable);
            i_frameLatencyWaitable = nullptr;
        }
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
        // Don't spend GPU on a hidden/minimized panel (e.g. tabbed
        // behind another AE panel). RenderFrame runs on AE's main UI
        // thread; every frame we draw here competes with AE's own GPU
        // work, and a blocked Present here freezes all of AE. No
        // point paying that cost when nothing's on screen.
        if (!IsWindowVisible(i_host) || IsIconic(i_host)) return;
        // THE freeze fix: only render when the swapchain's frame-
        // latency waitable says the GPU has consumed the previous
        // frame. Poll with timeout 0 (NON-blocking) — if it isn't
        // ready we skip this tick entirely rather than ever blocking
        // AE's UI thread inside Present/the GPU driver. This is the
        // single guarantee that our panel can't wedge all of AE.
        if (i_frameLatencyWaitable &&
            WaitForSingleObject(i_frameLatencyWaitable, 0) != WAIT_OBJECT_0)
        {
            return;
        }
        if (i_frame_in_progress) return;   // skip reentrant calls
        i_frame_in_progress = true;
        ImGui::SetCurrentContext(i_imguiCtx);

        // Per-INSTANCE first-frame logging (not process-global static)
        // so every renderer — including ones created on panel reopen —
        // logs its own first render. Each step is bracketed on the
        // first frame so a hang pinpoints the stuck call.
        const bool first = !i_logged_first_frame;
        if (first) {
            i_logged_first_frame = true;
            CM_DIAG_LOG("RenderFrame: first frame begin (renderer=%p)",
                        (void*)this);
        }

        if (first) CM_DIAG_LOG("RenderFrame: -> EnsureThumbnailTextures");
        EnsureThumbnailTextures();
        if (first) CM_DIAG_LOG("RenderFrame: <- EnsureThumbnailTextures");
        UpdateChasePreviewTexture();
        if (first) CM_DIAG_LOG("RenderFrame: <- UpdateChasePreviewTexture");

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT rc;
        GetClientRect(i_host, &rc);
        if (first) CM_DIAG_LOG("RenderFrame: -> panel_ui::RenderFrame");
        panel_ui::RenderFrame(i_state,
            static_cast<float>(rc.right - rc.left),
            static_cast<float>(rc.bottom - rc.top),
            i_host,
            "Windows / DX11");
        if (first) CM_DIAG_LOG("RenderFrame: <- panel_ui::RenderFrame");

        ImGui::Render();

        const float clearColor[4] = {0.10f, 0.10f, 0.11f, 1.0f};
        i_context->OMSetRenderTargets(1, &i_rtv, nullptr);
        i_context->ClearRenderTargetView(i_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // NON-BLOCKING present. This runs on AE's main UI thread
        // (WM_TIMER/WM_PAINT -> our subclassed WndProc -> RenderFrame).
        // Two confirmed hang dumps showed the AE main thread parked
        // inside the NVIDIA D3D UMD (nvwgf2umx) via dxgi!Present from
        // this exact call: when AE is saturating the GPU (rendering
        // the EXRDemux comp), Present blocks here and freezes ALL of
        // AE. Sync interval 0 didn't help (the wait is the busy
        // present-queue/driver, not vblank). DXGI_PRESENT_DO_NOT_WAIT
        // makes Present return DXGI_ERROR_WAS_STILL_DRAWING instead
        // of blocking the host thread — we just drop the frame and
        // try again on the next timer tick. Never let our panel's
        // Present stall AE's UI thread.
        // DXGI_ERROR_WAS_STILL_DRAWING here just means "GPU/present
        // queue busy (AE is rendering) — frame intentionally dropped";
        // it is NOT an error and we still fall through to reset state
        // + drain deferred UI actions so file dialogs / session I/O
        // aren't starved while the GPU is hot.
        HRESULT pr = i_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);

        if (pr != DXGI_ERROR_WAS_STILL_DRAWING && !i_logged_first_present) {
            i_logged_first_present = true;
            CM_DIAG_LOG("RenderFrame: first Present done (renderer=%p)",
                        (void*)this);
        }

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
        // CRITICAL: PanelState (and its LayerInfo::texture_id) outlives
        // this renderer — it survives panel close/reopen. The SRVs we
        // just released belong to THIS renderer's D3D device. If we
        // leave the ids non-zero, the next renderer (new device) sees
        // `texture_id != 0`, skips re-upload, and ImGui draws a freed
        // cross-device handle → AE render-manager crash. Zero them so
        // a fresh renderer always re-uploads on its own device.
        if (i_state) {
            CM_DIAG_LOG("ReleaseThumbs: acquiring state.mu");
            std::lock_guard<std::mutex> lk(i_state->mu);
            CM_DIAG_LOG("ReleaseThumbs: state.mu held; zeroing texture ids");
            for (auto& src : i_state->sources) {
                for (auto& L : src.layers) L.texture_id = 0;
            }
            i_state->chase_composite_texture_id = 0;
            CM_DIAG_LOG("ReleaseThumbs: done");
        }
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
    HANDLE                  i_frameLatencyWaitable = nullptr;
    ID3D11RenderTargetView* i_rtv = nullptr;
    ImGuiContext*           i_imguiCtx = nullptr;
    bool                    i_ready = false;
    bool                    i_frame_in_progress = false;
    bool                    i_in_dialog = false;
    bool                    i_logged_first_frame = false;
    bool                    i_logged_first_present = false;
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
    CM_DIAG_LOG("CreatePanelRenderer: container=%p isWindow=%d state=%p",
                container, hwnd ? (int)IsWindow(hwnd) : -1, (void*)state);
    if (!hwnd || !IsWindow(hwnd) || !state) return nullptr;
    return new WinPanelRenderer(hwnd, state);
}
