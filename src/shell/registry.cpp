#include "shell/registry.h"

#include "util/log.h"

#include <string>

namespace volchay::shell {

const wchar_t* const kRegisteredExtensions[] = {
    L".mp4", L".mov", L".mkv", L".webm", L".avi", L".m4v", L".wmv",
};
const int kRegisteredExtensionCount =
    int(sizeof(kRegisteredExtensions) / sizeof(kRegisteredExtensions[0]));

namespace {

// ProgID we own. Must be unique enough to avoid collisions.
constexpr wchar_t kProgId[]      = L"Volchay.Fastcut.Video.1";
constexpr wchar_t kVerbName[]    = L"OpenWithVolchayFastcut";
constexpr wchar_t kVerbDisplay[] = L"Open with Volchay-fastcut";
constexpr wchar_t kProgIdDesc[]  = L"Volchay-fastcut video";

std::wstring resolve_exe_path(const wchar_t* override_path) {
    if (override_path && *override_path) return std::wstring(override_path);
    wchar_t buf[MAX_PATH] = L"";
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::wstring self(buf, n);

    // Prefer "volchay-fastcut.exe" in the same directory if we can find
    // it. The registrar utility itself is a console app and must NOT be
    // pointed to from the context menu; otherwise clicking the entry
    // just flashes a console window and exits.
    size_t slash = self.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos)
                       ? std::wstring{}
                       : self.substr(0, slash + 1);
    std::wstring candidate = dir + L"volchay-fastcut.exe";
    DWORD attrs = ::GetFileAttributesW(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES
        && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return candidate;
    }
    return self;
}

LONG set_str(HKEY base, const std::wstring& sub,
             const wchar_t* name, const std::wstring& value) {
    HKEY h = nullptr;
    LONG e = ::RegCreateKeyExW(base, sub.c_str(), 0, nullptr, 0,
                               KEY_WRITE, nullptr, &h, nullptr);
    if (e != ERROR_SUCCESS) return e;
    e = ::RegSetValueExW(h, name, 0, REG_SZ,
        (const BYTE*)value.c_str(),
        DWORD((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(h);
    return e;
}

void delete_tree(HKEY base, const std::wstring& sub) {
    ::RegDeleteTreeW(base, sub.c_str());
}

LONG ensure_progid(HKEY base, const std::wstring& exe) {
    const std::wstring root = std::wstring(L"Software\\Classes\\") + kProgId;
    LONG e = set_str(base, root, nullptr, kProgIdDesc);
    if (e != ERROR_SUCCESS) return e;

    // Default icon = first icon resource from the exe.
    set_str(base, root + L"\\DefaultIcon", nullptr,
            std::wstring(L"\"") + exe + L"\",0");

    // Open command.
    const std::wstring cmd = std::wstring(L"\"") + exe + L"\" \"%1\"";
    set_str(base, root + L"\\shell\\open\\command", nullptr, cmd);

    // Friendly name on the verb.
    set_str(base, root + L"\\shell\\open", L"FriendlyAppName",
            L"Volchay-fastcut");

    return ERROR_SUCCESS;
}

LONG register_extension(HKEY base, const wchar_t* ext) {
    const std::wstring root =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY h = nullptr;
    LONG e = ::RegCreateKeyExW(base, root.c_str(), 0, nullptr, 0,
                               KEY_WRITE, nullptr, &h, nullptr);
    if (e != ERROR_SUCCESS) return e;
    DWORD zero = 0;
    e = ::RegSetValueExW(h, kProgId, 0, REG_NONE,
                         reinterpret_cast<const BYTE*>(&zero), 0);
    ::RegCloseKey(h);
    if (e != ERROR_SUCCESS) return e;

    // Install a "shell\verb" entry under SystemFileAssociations so the
    // command appears in the right-click menu directly (without requiring
    // the user to pick "Open with...").
    const std::wstring verb_root =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\")
        + ext + L"\\shell\\" + kVerbName;
    set_str(base, verb_root, nullptr, kVerbDisplay);
    set_str(base, verb_root, L"Icon",
            std::wstring(L"\"") + resolve_exe_path(nullptr) + L"\",0");

    // The command itself reuses the ProgID's open command, but we also
    // expose it explicitly so renaming the exe later still works once
    // re-registered.
    const std::wstring cmd_path = verb_root + L"\\command";
    const std::wstring cmd =
        std::wstring(L"\"") + resolve_exe_path(nullptr) + L"\" \"%1\"";
    set_str(base, cmd_path, nullptr, cmd);
    return ERROR_SUCCESS;
}

LONG unregister_extension(HKEY base, const wchar_t* ext) {
    // Remove only our ProgID from OpenWithProgids; never delete the whole
    // extension key (other apps may rely on it).
    const std::wstring root =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY h = nullptr;
    if (::RegOpenKeyExW(base, root.c_str(), 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        ::RegDeleteValueW(h, kProgId);
        ::RegCloseKey(h);
    }
    delete_tree(base,
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\")
        + ext + L"\\shell\\" + kVerbName);
    // Also remove any older registration that lived directly under the
    // extension's own \shell tree (some earlier builds wrote there).
    delete_tree(base,
        std::wstring(L"Software\\Classes\\")
        + ext + L"\\shell\\" + kVerbName);
    return ERROR_SUCCESS;
}

}  // namespace

LONG register_context_menu(bool per_user, const wchar_t* exe_path) {
    const std::wstring exe = resolve_exe_path(exe_path);
    if (exe.empty()) return ERROR_FILE_NOT_FOUND;

    HKEY base = per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;

    LONG e = ensure_progid(base, exe);
    if (e != ERROR_SUCCESS) return e;

    for (int i = 0; i < kRegisteredExtensionCount; ++i) {
        e = register_extension(base, kRegisteredExtensions[i]);
        if (e != ERROR_SUCCESS) return e;
    }

    // Notify the shell so the menu refreshes immediately.
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ERROR_SUCCESS;
}

LONG unregister_context_menu(bool per_user) {
    // Unregister from BOTH scopes. The flag is preserved for API compat,
    // but a partial cleanup leaves dangling entries in the other scope
    // (e.g. when the user registered with --machine but unregisters
    // without it). Since we only ever touch keys we own, this is safe.
    (void)per_user;
    HKEY scopes[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (HKEY base : scopes) {
        for (int i = 0; i < kRegisteredExtensionCount; ++i) {
            unregister_extension(base, kRegisteredExtensions[i]);
        }
        delete_tree(base, std::wstring(L"Software\\Classes\\") + kProgId);
    }
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ERROR_SUCCESS;
}

}  // namespace volchay::shell
