#pragma once

#include <cstdint>
#include <optional>
#include <string>

/// A native "open file" dialog.
///
/// The renderer has no UI toolkit beyond Dear ImGui, which ships no file
/// browser, so the mesh import asks the operating system instead. Windows is
/// implemented (the platform this project is developed on); elsewhere
/// `available()` returns false and the caller falls back to the path field in
/// the import window.
namespace filedialog {

/// True when `openMeshFile` can show a dialog on this platform.
bool available();

/// Shows a modal open dialog and returns the chosen path, or nothing when the
/// user cancelled or no dialog is available.
///
/// `ownerWindow` is the native handle of the window the dialog should belong to
/// (see Window::nativeHandle), so the dialog does not end up behind it.
std::optional<std::string> openMeshFile(const std::string& title, void* ownerWindow);

}  // namespace filedialog
