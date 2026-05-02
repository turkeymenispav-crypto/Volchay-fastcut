// Helpers for registering / unregistering Volchay-fastcut as a Windows
// "Open with" + right-click context menu handler for common video files.
//
// We use the legacy registry approach (HKEY_CURRENT_USER\Software\Classes)
// because:
//   * it does not require admin rights when we install per-user;
//   * it works in Windows 11's "Show more options" submenu and is the only
//     supported customisation for non-MSIX-packaged apps in Win11.
//
// A future iteration will add a Sparse MSIX package + IExplorerCommand
// COM handler so the entry shows up in the new Windows 11 menu directly.
#pragma once

#include "platform/windows.h"

namespace volchay::shell {

// Register the user's copy of volchay-fastcut.exe (auto-detected from the
// running executable's path, or override via exe_path).
//
// per_user = true   -> writes under HKCU (no admin required, recommended).
// per_user = false  -> writes under HKLM (requires elevation).
//
// Returns ERROR_SUCCESS on success, otherwise a Win32 error code.
LONG register_context_menu(bool per_user, const wchar_t* exe_path = nullptr);

// Remove the entries created by register_context_menu().
LONG unregister_context_menu(bool per_user);

// Internal: list of file extensions we register for.
extern const wchar_t* const kRegisteredExtensions[];
extern const int            kRegisteredExtensionCount;

}  // namespace volchay::shell
