#pragma once

#include <windows.h>
#include <string>

namespace st {

//: Directory holding the module that this code is linked into, with a
//: trailing backslash.  Resolved from the address of a function inside this
//: module rather than from GetModuleFileName(nullptr): a SAPI5 engine is a
//: DLL loaded into someone else's process, so the executable's directory is
//: whatever host happened to load us and is never where our data lives.
[[nodiscard]] std::wstring module_dir();

//: %APPDATA%\SmoothTalker\ -- per-user, roaming, writable without elevation.
//: Settings live here rather than in the registry (the engine is registry
//: free by design) or next to the DLL (Program Files is read-only to a
//: standard user).  Created on demand.
[[nodiscard]] std::wstring settings_dir();

//: %LOCALAPPDATA%\SmoothTalker\Logs\ -- machine-local, not worth roaming.
//: Created on demand.
[[nodiscard]] std::wstring log_dir();

//: Full path of the settings file.
[[nodiscard]] std::wstring settings_path();

//: Leaf name of the running process, e.g. L"nvda.exe".  Logged so a shared
//: log makes it obvious which host loaded the engine.
[[nodiscard]] std::wstring process_name();

//: Create `path` and every missing parent.  Returns false only if the
//: directory still does not exist afterwards.
bool ensure_dir(const std::wstring& path);

}
