#pragma once

//: Runtime binding to unicorn.dll.
//:
//: The SAPI5 engine is a DLL loaded into whatever process wants to speak --
//: a screen reader, Word, a book reader -- and those processes have their own
//: working directory and their own DLL search order.  An import-table
//: dependency on unicorn.dll would therefore be resolved (or not) against a
//: search path we do not control, and a failure would surface as a bare
//: "the specified module could not be found" at load time with no way to say
//: which module.  Binding by hand from an absolute path next to our own
//: module removes both problems and lets every failure name its file.

#include <windows.h>

#include <cstdint>
#include <string>

#include <unicorn/unicorn.h>

namespace st {

struct unicorn_api {
    uc_err (*open)(uc_arch, uc_mode, uc_engine**) = nullptr;
    uc_err (*close)(uc_engine*) = nullptr;
    uc_err (*mem_map)(uc_engine*, uint64_t, uint64_t, uint32_t) = nullptr;
    uc_err (*mem_write)(uc_engine*, uint64_t, const void*, uint64_t) = nullptr;
    uc_err (*mem_read)(uc_engine*, uint64_t, void*, uint64_t) = nullptr;
    uc_err (*reg_write)(uc_engine*, int, const void*) = nullptr;
    uc_err (*reg_read)(uc_engine*, int, void*) = nullptr;
    uc_err (*emu_start)(uc_engine*, uint64_t, uint64_t, uint64_t,
                        size_t) = nullptr;
    uc_err (*emu_stop)(uc_engine*) = nullptr;
    uc_err (*hook_add)(uc_engine*, uc_hook*, int, void*, void*, uint64_t,
                       uint64_t, ...) = nullptr;
    const char* (*strerror)(uc_err) = nullptr;
    unsigned int (*version)(unsigned int*, unsigned int*) = nullptr;

    [[nodiscard]] bool ok() const noexcept { return open != nullptr; }
};

//: Load unicorn.dll once per process and return the bound entry points, or
//: nullptr if it could not be loaded.  Every candidate path tried and every
//: failure is written to the log.  Thread safe; the DLL is never unloaded.
[[nodiscard]] const unicorn_api* unicorn_load();

//: Why the last unicorn_load() failed, for surfacing to the user.  Empty
//: while unicorn is working.
[[nodiscard]] std::wstring unicorn_last_error();

}
