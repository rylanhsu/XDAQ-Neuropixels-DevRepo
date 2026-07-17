// Resolves the DELAY_LOAD_DLL_NAME dependency relative to this module's own
// directory instead of relying on the host application's DLL search path
// (see /DELAYLOAD linker flag). If the DLL is already loaded elsewhere in the
// process, that instance is reused instead of loading a second copy, so all
// consumers in the same process share one instance.
#ifdef _WIN32

#include <windows.h>

#include <delayimp.h>

#include <cstring>
#include <string>

// The plugin is installed in <root>/<plugins dir>/ and its runtime
// dependencies in the sibling <root>/<shared dir>/. The Open Ephys GUI
// suffixes both directories with the plugin API version (e.g. "plugins-api10"
// and "shared-api10"), so the shared directory name is derived at runtime
// from this module's own parent directory name by swapping the prefixes
// below (e.g. "plugins-api10" -> "shared-api10").
#ifndef DELAY_LOAD_PLUGIN_DIR_PREFIX
#define DELAY_LOAD_PLUGIN_DIR_PREFIX L"plugins"
#endif
#ifndef DELAY_LOAD_SHARED_DIR_PREFIX
#define DELAY_LOAD_SHARED_DIR_PREFIX L"shared"
#endif

namespace
{

std::wstring moduleDirectory()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleDirectory),
        &module
    );
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring result(path, len);
    auto pos = result.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : result.substr(0, pos);
}

// Maps e.g. <root>/plugins-api10 to <root>/shared-api10, preserving whatever
// suffix the host appended to the plugins directory. Returns an empty string
// if this module is not in a "plugins*" directory.
std::wstring sharedDirectory()
{
    const std::wstring moduleDir = moduleDirectory();
    const auto pos = moduleDir.find_last_of(L"\\/");
    const std::wstring parent = pos == std::wstring::npos ? L"" : moduleDir.substr(0, pos);
    const std::wstring dirName = pos == std::wstring::npos ? moduleDir : moduleDir.substr(pos + 1);

    const std::wstring pluginPrefix = DELAY_LOAD_PLUGIN_DIR_PREFIX;
    if (dirName.compare(0, pluginPrefix.size(), pluginPrefix) != 0) return {};

    const std::wstring suffix = dirName.substr(pluginPrefix.size());
    return parent + L"/" + DELAY_LOAD_SHARED_DIR_PREFIX + suffix;
}

FARPROC WINAPI delayLoadHook(unsigned reason, PDelayLoadInfo info)
{
    if (reason != dliNotePreLoadLibrary || _stricmp(info->szDll, DELAY_LOAD_DLL_NAME) != 0)
        return nullptr;

    // Reuse an already-loaded copy (e.g. loaded by another XDAQ plugin in the
    // same process) so both share a single instance / device lock state.
    if (HMODULE existing = GetModuleHandleA(info->szDll))
        return reinterpret_cast<FARPROC>(existing);

    std::wstring dllName(info->szDll, info->szDll + std::strlen(info->szDll));

    // <root>/shared<suffix>/<dll>, derived from <root>/plugins<suffix>/ where
    // this module lives.
    if (const std::wstring sharedDir = sharedDirectory(); !sharedDir.empty()) {
        const std::wstring fullPath = sharedDir + L"/" + dllName;
        if (HMODULE loaded =
                LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR))
            return reinterpret_cast<FARPROC>(loaded);
    }

    // Fall back to this module's own directory (e.g. a developer build where
    // everything sits side by side).
    const std::wstring fallback = moduleDirectory() + L"/" + dllName;
    return reinterpret_cast<FARPROC>(
        LoadLibraryExW(fallback.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
    );
}

// Without a failure hook, the delay-load runtime raises a structured
// exception that takes down the whole host GUI. Show what is missing and let
// the plugin fail to load instead.
FARPROC WINAPI delayLoadFailureHook(unsigned reason, PDelayLoadInfo info)
{
    if (reason == dliFailLoadLib && _stricmp(info->szDll, DELAY_LOAD_DLL_NAME) == 0) {
        std::string message = "XDAQ-Neuropixels: failed to load required library \"";
        message += info->szDll;
        message += "\".\n\nExpected location: <Open Ephys>/shared<-apiN>/";
        message += info->szDll;
        MessageBoxA(nullptr, message.c_str(), "XDAQ-Neuropixels plugin", MB_OK | MB_ICONERROR);
    }
    return nullptr;
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = delayLoadHook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = delayLoadFailureHook;

#endif // _WIN32
