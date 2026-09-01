#include "file_dialog.h"

#include <windows.h>
#include <commdlg.h>

#include <string>
#include <vector>

namespace {

std::string Utf16ToUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');  // -1: drop trailing NUL
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

namespace file_dialog {

std::string PickExr(void* parent)
{
    HWND owner = static_cast<HWND>(parent);

    // Filename buffer: long enough for production paths, no Unicode
    // surprises. GetOpenFileNameW writes both the path and (if
    // OFN_EXPLORER) a single trailing NUL.
    std::vector<wchar_t> buf(4096, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  =
        L"Supported (EXR/image/movie)\0"
        L"*.exr;*.png;*.tif;*.tiff;*.jpg;*.jpeg;*.tga;*.dpx;*.hdr;*.mov;*.mp4;*.mxf\0"
        L"OpenEXR (*.exr)\0*.exr\0"
        L"Still images (*.png;*.tif;*.jpg;*.tga;*.dpx;*.hdr)\0"
        L"*.png;*.tif;*.tiff;*.jpg;*.jpeg;*.tga;*.dpx;*.hdr\0"
        L"Movies (*.mov;*.mp4;*.mxf)\0*.mov;*.mp4;*.mxf\0"
        L"All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = buf.data();
    ofn.nMaxFile     = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle   = L"Pick an EXR, image, or movie file";
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER |
                       OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return {};  // cancelled or error
    }
    return Utf16ToUtf8(buf.data());
}

// Helper: UTF-8 -> UTF-16, written into a buffer (truncates if needed).
static void Utf8ToWChar(const std::string& in, wchar_t* out, size_t out_cap)
{
    if (out_cap == 0) return;
    out[0] = L'\0';
    if (in.empty()) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, in.c_str(),
                                  static_cast<int>(in.size()),
                                  out, static_cast<int>(out_cap - 1));
    if (len < 0) len = 0;
    out[len] = L'\0';
}

std::string PickSessionSavePath(void* parent,
                                const std::string& default_basename)
{
    HWND owner = static_cast<HWND>(parent);
    std::vector<wchar_t> buf(4096, L'\0');
    if (!default_basename.empty()) {
        // Pre-populate with AE project basename + the session
        // extension so the user just hits Enter to save next to it
        // (or navigates elsewhere with the filename already filled).
        std::string seed = default_basename + ".chasemaker.json";
        Utf8ToWChar(seed, buf.data(), buf.size());
    } else {
        wcscpy_s(buf.data(), buf.size(), L"session.chasemaker.json");
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = L"Chase Maker session (*.chasemaker.json)\0*.chasemaker.json\0"
                       L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = buf.data();
    ofn.nMaxFile     = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle   = L"Save Chase Maker session";
    ofn.lpstrDefExt  = L"chasemaker.json";
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT |
                       OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return {};
    return Utf16ToUtf8(buf.data());
}

std::string PickSessionLoadPath(void* parent)
{
    HWND owner = static_cast<HWND>(parent);
    std::vector<wchar_t> buf(4096, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = L"Chase Maker session (*.chasemaker.json)\0*.chasemaker.json\0"
                       L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = buf.data();
    ofn.nMaxFile     = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle   = L"Load Chase Maker session";
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                       OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};
    return Utf16ToUtf8(buf.data());
}

} // namespace file_dialog
