// macOS implementation of PanelRenderer: hosts an MTKView as a
// subview of AE's container NSView, sets up Metal, and drives a Dear
// ImGui frame via the MTKView delegate's per-frame callback.
//
// Compiled with -fobjc-arc (see CMakeLists.txt). The C++ class holds
// __strong references to the ObjC objects; releasing the C++ object
// releases them in turn.

#include "panel_renderer.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

@class ChaseMakerMTKViewDelegate;

namespace {

class MacPanelRenderer : public PanelRenderer
{
public:
    explicit MacPanelRenderer(NSView* container);
    ~MacPanelRenderer() override;

    void RenderFrame(MTKView* view);

private:
    void InitImGui();
    void ShutdownImGui();

    NSView* __strong                   i_container = nil;
    MTKView* __strong                  i_mtkView = nil;
    id<MTLDevice> __strong             i_device = nil;
    id<MTLCommandQueue> __strong       i_commandQueue = nil;
    ChaseMakerMTKViewDelegate* __strong i_delegate = nil;
    ImGuiContext*                      i_imguiCtx = nullptr;
};

} // namespace

@interface ChaseMakerMTKViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithRenderer:(MacPanelRenderer*)renderer;
@end

@implementation ChaseMakerMTKViewDelegate {
    MacPanelRenderer* _renderer;
}

- (instancetype)initWithRenderer:(MacPanelRenderer*)renderer
{
    if ((self = [super init])) {
        _renderer = renderer;
    }
    return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view; (void)size;
}

- (void)drawInMTKView:(MTKView*)view
{
    if (_renderer) {
        _renderer->RenderFrame(view);
    }
}

@end

namespace {

MacPanelRenderer::MacPanelRenderer(NSView* container)
    : i_container(container)
{
    i_device = MTLCreateSystemDefaultDevice();
    if (!i_device) return;

    i_commandQueue = [i_device newCommandQueue];

    NSRect bounds = [i_container bounds];
    i_mtkView = [[MTKView alloc] initWithFrame:bounds device:i_device];
    i_mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    i_mtkView.clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
    i_mtkView.preferredFramesPerSecond = 60;
    i_mtkView.enableSetNeedsDisplay = NO;        // continuous redraw
    i_mtkView.paused = NO;
    i_mtkView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    i_delegate = [[ChaseMakerMTKViewDelegate alloc] initWithRenderer:this];
    i_mtkView.delegate = i_delegate;

    [i_container addSubview:i_mtkView];

    InitImGui();
}

MacPanelRenderer::~MacPanelRenderer()
{
    if (i_mtkView) {
        i_mtkView.paused = YES;
        i_mtkView.delegate = nil;
        [i_mtkView removeFromSuperview];
    }
    ShutdownImGui();
    // __strong members release on destruction.
}

void MacPanelRenderer::InitImGui()
{
    IMGUI_CHECKVERSION();
    i_imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(i_imguiCtx);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplMetal_Init(i_device);
    ImGui_ImplOSX_Init(i_mtkView);
}

void MacPanelRenderer::ShutdownImGui()
{
    if (!i_imguiCtx) return;
    ImGui::SetCurrentContext(i_imguiCtx);
    ImGui_ImplOSX_Shutdown();
    ImGui_ImplMetal_Shutdown();
    ImGui::DestroyContext(i_imguiCtx);
    i_imguiCtx = nullptr;
}

void MacPanelRenderer::RenderFrame(MTKView* view)
{
    if (!i_imguiCtx) return;
    ImGui::SetCurrentContext(i_imguiCtx);

    MTLRenderPassDescriptor* rpd = view.currentRenderPassDescriptor;
    if (!rpd) return;

    id<MTLCommandBuffer> cmd = [i_commandQueue commandBuffer];

    ImGui_ImplMetal_NewFrame(rpd);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();

    // Hello-world content. Single full-panel window pinned to the
    // MTKView's drawable size so layout follows panel resizes.
    const CGSize size = view.drawableSize;
    const CGFloat scale = view.window.backingScaleFactor ?: 1.0;
    const ImVec2 imSize(
        static_cast<float>(size.width / scale),
        static_cast<float>(size.height / scale));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(imSize);
    ImGui::Begin("ChaseMakerRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Text("Hello, Chase Maker (macOS / Metal)");
    ImGui::Text("Panel: %.0f x %.0f", imSize.x, imSize.y);
    ImGui::End();

    ImGui::Render();

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
    [enc endEncoding];

    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];
}

} // namespace

PanelRenderer* CreatePanelRenderer(void* container)
{
    if (!container) return nullptr;
    NSView* view = (__bridge NSView*)container;
    return new MacPanelRenderer(view);
}
