// Cross-platform "pick a .exr file" dialog. Blocking.
//
// `parent` is the panel's container view as a void* — HWND on
// Windows, NSView* on macOS. The dialog is made modal to its window.
//
// Returns the selected path as UTF-8, or empty string if the user
// cancelled (or any failure).

#pragma once

#include <string>

namespace file_dialog {

std::string PickExr(void* parent);

}
