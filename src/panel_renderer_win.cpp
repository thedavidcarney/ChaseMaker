// Windows implementation of PanelRenderer: subclasses AE's container
// HWND (the AEGP_PlatformViewRef), sets up a DX11 swap chain on it,
// and drives a Dear ImGui frame on every WM_PAINT plus a ~60fps timer.
//
// We follow the SDK Panelator pattern of subclassing the container
// rather than creating a child HWND — simpler, no resize forwarding
// needed, and AE's own paint stays out of the way once we stop
// chaining WM_PAINT to the old proc.

#include "panel_renderer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
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

class WinPanelRenderer : public PanelRenderer
{
public:
    explicit WinPanelRenderer(HWND host)
        : i_host(host)
    {
        if (!CreateDeviceAndSwapChain()) {
            Cleanup();
            return;
        }
        InitImGui();
        Subclass();

        // Drive ~60 Hz redraws so ImGui animations (cursor blink,
        // hover transitions) run smoothly. If this proves too chatty
        // later we can switch to redraw-on-demand only.
        SetTimer(i_host, kRedrawTimerId, kRedrawIntervalMs, nullptr);

        i_ready = true;
    }

    ~WinPanelRenderer() override
    {
        KillTimer(i_host, kRedrawTimerId);
        Unsubclass();
        ShutdownImGui();
        Cleanup();
    }

    LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (i_imguiCtx) {
            ImGui::SetCurrentContext(i_imguiCtx);
            if (ImGui_ImplWin32_WndProcHandler(i_host, msg, wparam, lparam)) {
                return 0;
            }
        }

        switch (msg) {
        case WM_PAINT: {
            RenderFrame();
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
        ImGui::SetCurrentContext(i_imguiCtx);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Hello-world content. Will be replaced by the real Chase
        // Maker UI in a later bite. Single full-panel window pinned
        // to the host's client area so the layout follows panel
        // resizes for free.
        RECT rc;
        GetClientRect(i_host, &rc);
        const ImVec2 size(
            static_cast<float>(rc.right - rc.left),
            static_cast<float>(rc.bottom - rc.top));
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("ChaseMakerRoot", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Text("Hello, Chase Maker (Windows / DX11)");
        ImGui::Text("Panel: %.0f x %.0f", size.x, size.y);
        ImGui::End();

        ImGui::Render();

        const float clearColor[4] = {0.10f, 0.10f, 0.11f, 1.0f};
        i_context->OMSetRenderTargets(1, &i_rtv, nullptr);
        i_context->ClearRenderTargetView(i_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        i_swapChain->Present(1, 0);
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
};

} // namespace

PanelRenderer* CreatePanelRenderer(void* container)
{
    HWND hwnd = static_cast<HWND>(container);
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    return new WinPanelRenderer(hwnd);
}
