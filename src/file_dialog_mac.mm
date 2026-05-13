#include "file_dialog.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

namespace file_dialog {

std::string PickExr(void* parent)
{
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.title = @"Pick a multilayer EXR";

        if (@available(macOS 11.0, *)) {
            UTType* exrType = [UTType typeWithFilenameExtension:@"exr"];
            if (exrType) panel.allowedContentTypes = @[exrType];
        }

        // Make the sheet modal to the host view's window if we can,
        // so it visually anchors to the AE panel. Falls back to a
        // free-floating modal dialog otherwise.
        NSView* hostView = (__bridge NSView*)parent;
        NSWindow* hostWindow = hostView ? hostView.window : nil;
        (void)hostWindow;  // currently using runModal regardless — see note

        // We use runModal (blocking, app-modal) rather than the sheet
        // form because the call happens inside the MTKView render
        // callback. Driving a sheet's completion handler back onto
        // the render thread is more plumbing than a hello-world flow
        // needs; the brief frame stall while the panel is open is
        // acceptable.
        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK) return {};

        NSURL* url = panel.URLs.firstObject;
        if (!url) return {};
        NSString* path = url.path;
        if (!path) return {};
        return std::string(path.UTF8String ? path.UTF8String : "");
    }
}

} // namespace file_dialog
