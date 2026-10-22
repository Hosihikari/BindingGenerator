// Preload-native LeviLamina mod that hosts the CLR and runs the symbol dispatch smoke.
//
// The DLL is loaded by PreLoader before the server starts. A worker thread waits for
// the server to finish starting, then resolves hostfxr through nethost and calls the
// managed entry point. The managed side loads the generated symbol dispatch shim and
// exercises the real BDS symbols by RVA.

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "coreclr_delegates.h"
#include "hostfxr.h"
#include "nethost.h"

namespace {

using run_managed_fn = component_entry_point_fn;

HMODULE g_module = nullptr;

std::filesystem::path module_directory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::filesystem::path text(path);
    return text.parent_path();
}

void write_error(std::filesystem::path const& path, std::string const& message) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary);
    output << "load=failed\n";
    output << "error=" << message << "\n";
    output << "result=FAIL\n";
}

bool host_and_run(std::filesystem::path const& directory, std::uintptr_t service, std::string& error) {
    auto const managedDll = directory / "Hosihikari.SymbolSmoke.dll";
    auto const runtimeConfig = directory / "Hosihikari.SymbolSmoke.runtimeconfig.json";
    auto const nethostPath = directory / "nethost.dll";

    if (!std::filesystem::exists(managedDll) || !std::filesystem::exists(runtimeConfig)) {
        error = "managed smoke payload was not found in " + directory.string();
        return false;
    }
    if (!std::filesystem::exists(nethostPath)) {
        error = "nethost.dll was not found in " + directory.string();
        return false;
    }

    HMODULE nethost = LoadLibraryW(nethostPath.c_str());
    if (nethost == nullptr) {
        error = "failed to load nethost.dll";
        return false;
    }

    auto getHostfxrPath = reinterpret_cast<decltype(&get_hostfxr_path)>(GetProcAddress(nethost, "get_hostfxr_path"));
    if (getHostfxrPath == nullptr) {
        error = "get_hostfxr_path was not exported by nethost.dll";
        return false;
    }

    wchar_t hostfxrPath[4096]{};
    size_t hostfxrPathSize = std::size(hostfxrPath);
    get_hostfxr_parameters parameters{};
    parameters.size = sizeof(parameters);
    parameters.assembly_path = managedDll.c_str();
    if (getHostfxrPath(hostfxrPath, &hostfxrPathSize, &parameters) != 0) {
        error = "get_hostfxr_path failed";
        return false;
    }

    HMODULE hostfxr = LoadLibraryW(hostfxrPath);
    if (hostfxr == nullptr) {
        error = "failed to load hostfxr.dll";
        return false;
    }

    auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto getRuntimeDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate"));
    auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(hostfxr, "hostfxr_close"));
    if (initialize == nullptr || getRuntimeDelegate == nullptr || close == nullptr) {
        error = "hostfxr entry points were not found";
        return false;
    }

    hostfxr_handle context = nullptr;
    if (initialize(runtimeConfig.c_str(), nullptr, &context) != 0 || context == nullptr) {
        error = "hostfxr_initialize_for_runtime_config failed";
        return false;
    }

    load_assembly_and_get_function_pointer_fn loadAssembly = nullptr;
    if (getRuntimeDelegate(context, hdt_load_assembly_and_get_function_pointer, reinterpret_cast<void**>(&loadAssembly))
            != 0
        || loadAssembly == nullptr) {
        close(context);
        error = "hostfxr_get_runtime_delegate failed";
        return false;
    }

    run_managed_fn run = nullptr;
    const char_t* typeName = L"Hosihikari.SymbolSmoke.NativeEntryPoint, Hosihikari.SymbolSmoke";
    if (loadAssembly(managedDll.c_str(), typeName, L"Run", UNMANAGEDCALLERSONLY_METHOD, nullptr,
                     reinterpret_cast<void**>(&run))
            != 0
        || run == nullptr) {
        close(context);
        error = "load_assembly_and_get_function_pointer failed";
        return false;
    }

    const int result = run(reinterpret_cast<void*>(service), 0);
    close(context);
    if (result != 0) {
        error = "managed symbol smoke returned a failure";
        return false;
    }

    return true;
}

DWORD WINAPI worker(LPVOID) {
    // Let the server finish starting; BlockPos does not depend on world state, but the
    // hostfxr and the game's formatter are safer once the main thread is past startup.
    Sleep(8000);

    auto const directory = module_directory();
    auto const shimPath = directory / "symbol-shim.dll";
    auto const resultPath = directory / "symbol-smoke-result.txt";
    SetEnvironmentVariableW(L"HS_SYMBOL_SHIM", shimPath.c_str());
    SetEnvironmentVariableW(L"HS_SYMBOL_RESULT", resultPath.c_str());

    // PeEditor exports the game's data symbols. ServiceLocator<T>::mService is a
    // NonOwnerPointer whose first pointer is the live service object. AppPlatform is a
    // server-side service with a real vtable, so the managed side can call a virtual
    // getter on it without constructing anything.
    std::uintptr_t platform = 0;
    HMODULE executable = GetModuleHandleW(nullptr);
    auto service = reinterpret_cast<void* const*>(GetProcAddress(
        executable,
        R"(?mService@?$ServiceLocator@VAppPlatform@@@@2V?$NonOwnerPointer@VAppPlatform@@@Bedrock@@A)"));
    if (service != nullptr) {
        platform = reinterpret_cast<std::uintptr_t>(*service);
    }

    {
        std::ofstream diagnostic(directory / "symbol-smoke-plugin.txt", std::ios::binary);
        diagnostic << "appplatform=0x" << std::hex << platform << std::dec << std::endl;
    }

    std::string error;
    if (!host_and_run(directory, platform, error) && !std::filesystem::exists(resultPath)) {
        write_error(resultPath, error);
    }

    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
    }

    return TRUE;
}
