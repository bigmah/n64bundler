// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opening a game module.
//
// The module is a dylib with one exported symbol and a great many undefined
// ones. dlopen binds the undefined ones against this executable, which is why
// the host is linked with -export_dynamic and why it force-loads the runtime
// archives: a symbol the host never calls itself still has to be there for the
// game to call.

#include "host.hpp"

#include <dlfcn.h>

#include <librecomp/game.hpp>

namespace n64b {

Module::~Module() {
    // Deliberately not dlclose'd. A module's static section table is still
    // referenced by librecomp's overlay tables at this point, and the process
    // is about to exit anyway; unloading it buys nothing and can only turn an
    // orderly shutdown into a crash inside the loader.
}

std::unique_ptr<Module> open_module(const std::string &path, std::string &error) {
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        error = std::string("could not open the game module: ") + dlerror();
        return nullptr;
    }

    auto *desc = static_cast<const n64b_module_v1 *>(dlsym(handle, N64B_MODULE_SYMBOL));
    if (desc == nullptr) {
        error = path + " is a library but not a game module: it has no " N64B_MODULE_SYMBOL;
        return nullptr;
    }

    if (desc->abi_version != N64B_MODULE_ABI) {
        error = "this game was ported by a different version of N64Bundler (module ABI " +
                std::to_string(desc->abi_version) + ", this host speaks " +
                std::to_string(N64B_MODULE_ABI) + "). Re-add the ROM to rebuild it.";
        return nullptr;
    }

    if (desc->entrypoint == nullptr || desc->code_sections == nullptr ||
        desc->num_code_sections == 0) {
        error = "the game module is missing its code; it was not built completely";
        return nullptr;
    }

    auto module = std::make_unique<Module>();
    module->handle = handle;
    module->desc = desc;
    return module;
}

int save_type_of(const n64b_module_v1 &desc) {
    switch (desc.save_type) {
        case N64B_SAVE_NONE: return int(recomp::SaveType::None);
        case N64B_SAVE_EEP4K: return int(recomp::SaveType::Eep4k);
        case N64B_SAVE_EEP16K: return int(recomp::SaveType::Eep16k);
        case N64B_SAVE_SRAM: return int(recomp::SaveType::Sram);
        case N64B_SAVE_FLASHRAM: return int(recomp::SaveType::Flashram);
        default: return int(recomp::SaveType::AllowAll);
    }
}

} // namespace n64b
