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
    ofn.lpstrFilter  = L"OpenEXR (*.exr)\0*.exr\0PNG (*.png)\0*.png\0All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = buf.data();
    ofn.nMaxFile     = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle   = L"Pick a multilayer EXR or PNG";
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER |
                       OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return {};  // cancelled or error
    }
    return Utf16ToUtf8(buf.data());
}

std::string PickSessionSavePath(void* parent)
{
    HWND owner = static_cast<HWND>(parent);
    std::vector<wchar_t> buf(4096, L'\0');
    wcscpy_s(buf.data(), buf.size(), L"session.chasemaker.json");

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
