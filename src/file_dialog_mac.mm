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
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            if (UTType* t = [UTType typeWithFilenameExtension:@"exr"]) [types addObject:t];
            if (UTType* t = [UTType typeWithFilenameExtension:@"png"]) [types addObject:t];
            if (types.count) panel.allowedContentTypes = types;
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

std::string PickSessionSavePath(void* parent)
{
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = @"Save Chase Maker session";
        panel.nameFieldStringValue = @"session.chasemaker.json";
        if (@available(macOS 11.0, *)) {
            if (UTType* t = [UTType typeWithFilenameExtension:@"json"]) {
                panel.allowedContentTypes = @[t];
            }
        }
        (void)parent;
        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK) return {};
        NSURL* url = panel.URL;
        if (!url) return {};
        NSString* path = url.path;
        if (!path) return {};
        return std::string(path.UTF8String ? path.UTF8String : "");
    }
}

std::string PickSessionLoadPath(void* parent)
{
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.title = @"Load Chase Maker session";
        if (@available(macOS 11.0, *)) {
            if (UTType* t = [UTType typeWithFilenameExtension:@"json"]) {
                panel.allowedContentTypes = @[t];
            }
        }
        (void)parent;
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
