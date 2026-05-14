// macOS implementation of PanelRenderer: hosts an MTKView as a
// subview of AE's container NSView, sets up Metal, and drives a Dear
// ImGui frame via the MTKView delegate's per-frame callback.
//
// Compiled with -fobjc-arc (see CMakeLists.txt). The C++ class holds
// __strong references to the ObjC objects; releasing the C++ object
// releases them in turn.

#include "panel_renderer.h"
#include "panel_state.h"
#include "panel_ui.h"
#include "file_dialog.h"
#include "exr_scan.h"

#include <atomic>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ClearActiveID
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

@class ChaseMakerMTKViewDelegate;

// Subclass MTKView so the view participates in the first-responder
// chain. Without this, the AE panel can host the view but key
// events never reach ImGui — the text-input field looks editable
// but rejects keystrokes.
@interface ChaseMakerMTKView : MTKView
@end

@implementation ChaseMakerMTKView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder  { return YES; }

// Accept the very first click even when our window isn't currently
// key. Without this, AppKit eats the first click as "activate window"
// and only the second click is delivered to the view — meaning the
// user has to click the input twice to get a usable text cursor
// when returning from another AE panel that's in a different window.
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

// On click, route firstResponder to ImGui's internal
// KeyEventResponder subview (added by ImGui_ImplOSX_Init). That
// subview's keyDown calls ImGui_ImplOSX_HandleEvent, and because it
// belongs to our window, the event.window filter passes. This is
// the fallback path for AE setups where the NSApp-level local
// monitor never sees the keys (AE evidently dispatches some keys
// via direct responder routing rather than through NSApp.sendEvent
// where local monitors observe).
- (void)mouseDown:(NSEvent*)event
{
    [super mouseDown:event];
    for (NSView* sub in self.subviews) {
        if ([sub conformsToProtocol:@protocol(NSTextInputClient)]) {
            [self.window makeFirstResponder:sub];
            break;
        }
    }
}
@end

namespace {

class MacPanelRenderer : public PanelRenderer
{
public:
    MacPanelRenderer(NSView* container, PanelState* state);
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
    id __strong                        i_consume_monitor = nil;
    ImGuiContext*                      i_imguiCtx = nullptr;
    PanelState*                        i_state = nullptr;
    bool                               i_imgui_inited = false;
    bool                               i_had_focus = false;
    std::atomic<bool>                  i_want_keys{false};
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

MacPanelRenderer::MacPanelRenderer(NSView* container, PanelState* state)
    : i_container(container), i_state(state)
{
    i_device = MTLCreateSystemDefaultDevice();
    if (!i_device) return;

    i_commandQueue = [i_device newCommandQueue];

    NSRect bounds = [i_container bounds];
    i_mtkView = [[ChaseMakerMTKView alloc] initWithFrame:bounds device:i_device];
    i_mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    i_mtkView.clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
    i_mtkView.preferredFramesPerSecond = 60;
    i_mtkView.enableSetNeedsDisplay = NO;        // continuous redraw
    i_mtkView.paused = NO;
    i_mtkView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    i_delegate = [[ChaseMakerMTKViewDelegate alloc] initWithRenderer:this];
    i_mtkView.delegate = i_delegate;

    [i_container addSubview:i_mtkView];

    // Don't InitImGui here — i_mtkView.window may still be nil at
    // this point (AE may not have placed the container in a window
    // yet). The OSX backend captures view.window at Init time and
    // uses it to filter key events, so initing too early routes
    // keystrokes nowhere. Lazy-init from RenderFrame, where we know
    // the view is in a window because Metal is drawing it.
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

    // Note: an earlier "pre_monitor" used to update
    // main_viewport->PlatformHandle = event.window for every event.
    // It was intended to make ImGui's filter pass for keys arriving
    // on a window other than our view's — but in practice keys
    // always arrive on our window (because KeyEventResponder is in
    // our view), and the pre-monitor's side effect was to ALSO
    // accept mouse events from unrelated AE windows, causing
    // spurious widget activations when the user clicked on the
    // Project panel. Removed; per-frame PlatformHandle = view.window
    // (set at the top of RenderFrame) is sufficient.

    // Note: we deliberately do NOT install a "consume" local
    // monitor here. Doing so was preventing character input —
    // local-monitor consumption blocks the responder chain, so
    // KeyEventResponder.keyDown never fires, and without that
    // we lose `interpretKeyEvents:` -> `insertText:` -> ImGui's
    // `AddInputCharactersUTF8`. Key events alone (down/up) come
    // from ImGui's own monitor, but actual typed characters
    // require the responder-chain path.
    //
    // Hotkey suppression instead relies on KeyEventResponder's
    // built-in behavior: its keyDown calls HandleEvent, which
    // returns io.WantCaptureKeyboard; on true, [super keyDown:]
    // is skipped, so the event does NOT propagate up to AE's
    // responders. When our input is focused, AE never sees the
    // key. When no input is focused, AE handles its hotkeys
    // normally.
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
    if (!i_imgui_inited) {
        if (!view.window) return;          // wait for AE to attach a window
        InitImGui();
        i_imgui_inited = true;
    }
    if (!i_imguiCtx) return;
    ImGui::SetCurrentContext(i_imguiCtx);

    // Re-publish the platform handle each frame to whatever window
    // the MTKView is currently in. ImGui_ImplOSX_HandleEvent filters
    // incoming key/mouse events by `event.window == main_viewport->PlatformHandle`,
    // and AE can re-parent the view between init time and when the
    // user actually types. Re-publishing fixes both cases: AE moves
    // the panel into a different window, or the window we captured
    // at init turns out not to be the key window when events fire.
    if (view.window) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        vp->PlatformHandle = vp->PlatformHandleRaw = (__bridge void*)view.window;
    }

    // Mirror Windows' WM_KILLFOCUS-driven ClearActiveID: detect when
    // input focus is no longer on us and tell ImGui to release
    // whichever widget is currently active. "On us" means BOTH our
    // window is the key window AND a view in our hierarchy is its
    // firstResponder. The window-key check catches the case where
    // the user clicked a separate AE window (e.g. an undocked
    // panel) — our window's firstResponder won't change because
    // that click went to a different window.
    bool focus_ours = false;
    if (view.window) {
        const bool is_key = [view.window isKeyWindow];
        NSResponder* fr = view.window.firstResponder;
        bool responder_ours = false;
        if ([fr isKindOfClass:[NSView class]]) {
            NSView* fv = (NSView*)fr;
            responder_ours = (fv == i_mtkView) || [fv isDescendantOf:i_mtkView];
        }
        focus_ours = is_key && responder_ours;
    }
    if (i_had_focus && !focus_ours) {
        ImGui::ClearActiveID();
    }
    i_had_focus = focus_ours;

    MTLRenderPassDescriptor* rpd = view.currentRenderPassDescriptor;
    if (!rpd) return;

    id<MTLCommandBuffer> cmd = [i_commandQueue commandBuffer];

    ImGui_ImplMetal_NewFrame(rpd);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();

    const CGSize size = view.drawableSize;
    const CGFloat scale = view.window.backingScaleFactor ?: 1.0;
    panel_ui::RenderFrame(i_state,
        static_cast<float>(size.width / scale),
        static_cast<float>(size.height / scale),
        (__bridge void*)i_mtkView,
        "macOS / Metal");

    ImGui::Render();

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
    [enc endEncoding];

    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];

    // Snapshot keyboard-capture intent for the consume-monitor. Read
    // here (after ImGui::Render set it for this frame) and the next
    // key event the monitor sees will use this value.
    const ImGuiIO& io = ImGui::GetIO();
    i_want_keys.store(io.WantCaptureKeyboard || io.WantTextInput);

    // Drain deferred UI actions AFTER the frame is fully submitted.
    // Same rationale as the Windows path: NSOpenPanel.runModal spins
    // its own event loop and we don't want ImGui's frame state to
    // be mid-update during that.
    if (i_state->want_pick_exr.exchange(false)) {
        i_mtkView.paused = YES;
        std::string path = file_dialog::PickExr((__bridge void*)i_mtkView);
        i_mtkView.paused = NO;
        if (!path.empty()) {
            exr_scan::StartScan(path, i_state);
        }
    }
    if (i_state->want_write_sidecar.exchange(false)) {
        exr_scan::WriteLuminositySidecar(i_state);
    }
}

} // namespace

PanelRenderer* CreatePanelRenderer(void* container, PanelState* state)
{
    if (!container || !state) return nullptr;
    NSView* view = (__bridge NSView*)container;
    return new MacPanelRenderer(view, state);
}
