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
#include "session_io.h"

#include <atomic>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ClearActiveID
#include "imgui_impl_osx.h"
#include "imgui_impl_metal.h"

@class ChaseMakerMTKViewDelegate;

namespace { class MacPanelRenderer; }

// Subclass MTKView so the view participates in the first-responder
// chain. Without this, the AE panel can host the view but key
// events never reach ImGui — the text-input field looks editable
// but rejects keystrokes.
//
// Also doubles as the drag destination — registers for file URLs in
// the C++ renderer's constructor and forwards drops back via
// `renderer` (assign, raw C++ pointer; renderer dtor sets it to nil).
@interface ChaseMakerMTKView : MTKView
@property (assign, nonatomic) MacPanelRenderer* renderer;
@end

// Forward-declared C++ helpers so the @implementation can call into
// MacPanelRenderer before its class body is in scope. Defined after
// MacPanelRenderer is complete.
namespace {
void DispatchDroppedPaths(MacPanelRenderer* renderer,
                          const std::vector<std::string>& paths);
void DispatchUnknownDrop(MacPanelRenderer* renderer, NSPasteboard* pb);
}

@implementation ChaseMakerMTKView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder  { return YES; }

// Accept the very first click even when our window isn't currently
// key. Without this, AppKit eats the first click as "activate window"
// and only the second click is delivered to the view — meaning the
// user has to click the input twice to get a usable text cursor
// when returning from another AE panel that's in a different window.
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

// ---- NSDraggingDestination (file-URL drops) ----
//
// The renderer registers our accepted types in its constructor; the
// methods below are wired up by the responder chain because we're
// the registered destination.

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    NSPasteboard* pb = [sender draggingPasteboard];
    if ([[pb types] containsObject:NSPasteboardTypeFileURL]) {
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSPasteboard* pb = [sender draggingPasteboard];
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    std::vector<std::string> paths;
    for (NSURL* url in urls) {
        if (url.fileURL && url.path) {
            const char* u8 = url.path.UTF8String;
            if (u8 && *u8) paths.emplace_back(u8);
        }
    }
    if (!paths.empty() && self.renderer) {
        DispatchDroppedPaths(self.renderer, paths);
        return YES;
    }
    if (self.renderer) {
        DispatchUnknownDrop(self.renderer, pb);
    }
    return NO;
}

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

    // Called by ChaseMakerMTKView when a file-URL drop completes.
    void OnDroppedFiles(const std::vector<std::string>& paths);
    // Called when a drop arrives without a file-URL format; logs the
    // available pasteboard types into state.last_status so we can
    // identify and add explicit support for AE's drag format.
    void OnUnknownDrop(NSPasteboard* pb);

private:
    void InitImGui();
    void ShutdownImGui();

    NSView* __strong                   i_container = nil;
    ChaseMakerMTKView* __strong        i_mtkView = nil;
    id<MTLDevice> __strong             i_device = nil;
    id<MTLCommandQueue> __strong       i_commandQueue = nil;
    ChaseMakerMTKViewDelegate* __strong i_delegate = nil;
    id __strong                        i_consume_monitor = nil;
    NSMutableArray<id<MTLTexture>>* __strong i_thumb_textures = nil;
    id<MTLTexture> __strong            i_chase_composite_tex = nil;
    int                                i_last_scan_gen = -1;
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
    i_thumb_textures = [NSMutableArray array];
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

    // Wire the drop destination — view forwards drops via the assign
    // back-pointer set here. Cleared in dtor before this destructs.
    i_mtkView.renderer = this;
    [i_mtkView registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

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
        i_mtkView.renderer = nullptr;
        [i_mtkView unregisterDraggedTypes];
        i_mtkView.paused = YES;
        i_mtkView.delegate = nil;
        [i_mtkView removeFromSuperview];
    }
    ShutdownImGui();
    // __strong members release on destruction.
}

void MacPanelRenderer::OnDroppedFiles(const std::vector<std::string>& paths)
{
    for (const auto& p : paths) {
        if (p.empty()) continue;
        exr_scan::StartScan(p, i_state, /*append=*/true, /*source_id=*/0);
    }
    if (!paths.empty() && i_state) {
        std::lock_guard<std::mutex> lk(i_state->mu);
        i_state->last_status = "Dropped " + std::to_string(paths.size()) +
                               " file(s) — scanning...";
        i_state->last_error.clear();
    }
}

void MacPanelRenderer::OnUnknownDrop(NSPasteboard* pb)
{
    if (!pb || !i_state) return;
    std::string msg = "Drop received but no file URL. Types:";
    NSArray<NSPasteboardType>* types = [pb types];
    for (NSPasteboardType t in types) {
        const char* c = t.UTF8String;
        if (c) { msg += " ["; msg += c; msg += "]"; }
    }
    std::lock_guard<std::mutex> lk(i_state->mu);
    i_state->last_status = msg;
}

void DispatchDroppedPaths(MacPanelRenderer* renderer,
                          const std::vector<std::string>& paths)
{
    if (renderer) renderer->OnDroppedFiles(paths);
}

void DispatchUnknownDrop(MacPanelRenderer* renderer, NSPasteboard* pb)
{
    if (renderer) renderer->OnUnknownDrop(pb);
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

    // ---- Chase preview composite texture ----
    if (i_state && i_state->chase_composite_dirty.exchange(false)) {
        i_chase_composite_tex = nil;
        const int cw = i_state->chase_composite_w;
        const int ch = i_state->chase_composite_h;
        if (cw > 0 && ch > 0 && !i_state->chase_composite_rgba.empty()) {
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:(NSUInteger)cw
                                                                  height:(NSUInteger)ch
                                                               mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            i_chase_composite_tex = [i_device newTextureWithDescriptor:td];
            if (i_chase_composite_tex) {
                [i_chase_composite_tex replaceRegion:MTLRegionMake2D(0, 0, cw, ch)
                                         mipmapLevel:0
                                           withBytes:i_state->chase_composite_rgba.data()
                                         bytesPerRow:cw * 4];
                i_state->chase_composite_texture_id =
                    (uint64_t)(uintptr_t)(__bridge void*)i_chase_composite_tex;
            } else {
                i_state->chase_composite_texture_id = 0;
            }
        } else {
            i_state->chase_composite_texture_id = 0;
        }
    }

    // ---- Thumbnail texture cache ----
    if (i_state) {
        const int gen = i_state->scan_generation.load();
        bool released = false;
        if (gen != i_last_scan_gen) {
            [i_thumb_textures removeAllObjects];
            i_chase_composite_tex = nil;
            i_state->chase_composite_texture_id = 0;
            i_last_scan_gen = gen;
            released = true;
        }
        std::lock_guard<std::mutex> lk(i_state->mu);
        if (released) {
            // SAME bug class as the Windows ReleaseAllThumbnailTextures
            // fix: the MTLTextures we just dropped belong to THIS
            // renderer, but PanelState (and LayerInfo::texture_id)
            // outlives it across panel close/reopen. If the ids stay
            // non-zero the next renderer's `if (L.texture_id != 0)
            // continue;` skips re-upload and ImGui draws a freed,
            // cross-renderer Metal texture → crash. Zero every id
            // (all sources) so a fresh renderer always re-uploads.
            for (auto& s : i_state->sources) {
                for (auto& L : s.layers) L.texture_id = 0;
            }
            i_state->chase_composite_texture_id = 0;
        }
        Source* src = ActiveSource(*i_state);
        if (src) for (LayerInfo& L : src->layers) {
            if (L.texture_id != 0) continue;
            if (L.thumb_rgba.empty() || L.thumb_w <= 0 || L.thumb_h <= 0) continue;
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:(NSUInteger)L.thumb_w
                                                                  height:(NSUInteger)L.thumb_h
                                                               mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> tex = [i_device newTextureWithDescriptor:td];
            if (!tex) continue;
            [tex replaceRegion:MTLRegionMake2D(0, 0, L.thumb_w, L.thumb_h)
                   mipmapLevel:0
                     withBytes:L.thumb_rgba.data()
                   bytesPerRow:L.thumb_w * 4];
            [i_thumb_textures addObject:tex];
            // Bridge the texture pointer through to a uint64 ImTextureID.
            // Lifetime is owned by i_thumb_textures; ImGui never deref's
            // the value itself.
            L.texture_id = (uint64_t)(uintptr_t)(__bridge void*)tex;
        }
    }

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
    auto run_dialog = [&](auto&& dialog_call) {
        i_mtkView.paused = YES;
        auto result = dialog_call();
        i_mtkView.paused = NO;
        return result;
    };
    if (i_state->want_pick_exr.exchange(false)) {
        std::string path = run_dialog([&](){
            return file_dialog::PickExr((__bridge void*)i_mtkView);
        });
        if (!path.empty()) {
            exr_scan::StartScan(path, i_state, /*append=*/true, /*source_id=*/0);
        }
    }
    if (i_state->want_save_session.exchange(false)) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(i_state->mu);
            path = i_state->session_save_path;
        }
        if (path.empty()) {
            path = run_dialog([&](){
                return file_dialog::PickSessionSavePath((__bridge void*)i_mtkView);
            });
        }
        if (!path.empty()) session_io::WriteSession(i_state, path);
    }
    if (i_state->want_load_session.exchange(false)) {
        std::string path = run_dialog([&](){
            return file_dialog::PickSessionLoadPath((__bridge void*)i_mtkView);
        });
        if (!path.empty()) session_io::LoadSession(i_state, path);
    }
    // want_build_* drained by the AEGP idle hook in chase_maker.cpp.
}

} // namespace

PanelRenderer* CreatePanelRenderer(void* container, PanelState* state)
{
    if (!container || !state) return nullptr;
    NSView* view = (__bridge NSView*)container;
    return new MacPanelRenderer(view, state);
}
