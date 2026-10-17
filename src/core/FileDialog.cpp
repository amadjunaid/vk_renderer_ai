#include "core/FileDialog.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h has to come first: commdlg.h uses CALLBACK, HWND and friends
// without pulling in the base header itself.
#include <windows.h>
#include <commdlg.h>
#endif

namespace filedialog {

#ifdef _WIN32

namespace {

/// Size of the buffer the dialog writes the chosen path into. The classic
/// MAX_PATH is enough because the dialog itself refuses longer paths unless the
/// process opts into long-path support.
constexpr int kPathBuffer = 1024;

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

}  // namespace

#endif  // _WIN32

bool available() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::optional<std::string> openMeshFile(const std::string& title, void* ownerWindow) {
#ifdef _WIN32
    // The filter is a sequence of NUL-separated pairs terminated by an extra
    // NUL, which the literal's own terminator supplies.
    const wchar_t filter[] = L"glTF binary (*.glb)\0*.glb\0All files (*.*)\0*.*\0";

    wchar_t pathBuffer[kPathBuffer] = {};
    const std::wstring wideTitle = toWide(title);

    OPENFILENAMEW openInfo{};
    openInfo.lStructSize = sizeof(openInfo);
    openInfo.hwndOwner = static_cast<HWND>(ownerWindow);
    openInfo.hInstance = GetModuleHandleW(nullptr);
    openInfo.lpstrFilter = filter;
    openInfo.lpstrFile = pathBuffer;
    openInfo.nMaxFile = kPathBuffer;
    openInfo.lpstrTitle = wideTitle.empty() ? nullptr : wideTitle.c_str();
    // OFN_NOCHANGEDIR matters: without it the dialog's directory becomes the
    // process working directory, which would move any later relative path.
    openInfo.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetOpenFileNameW(&openInfo) == FALSE) {
        return std::nullopt;  // cancelled, or the dialog failed to open
    }
    return toUtf8(pathBuffer);
#else
    (void)title;
    (void)ownerWindow;
    return std::nullopt;
#endif
}

}  // namespace filedialog
