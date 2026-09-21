// Link-time stubs for the desktop live-recompiler objects. The Switch build
// reports live hooks as unavailable in mods.cpp, but the ABI-owned containers
// still need their destructors and shim lifetime symbols defined.
#include "recompiler/live_recompiler.h"

namespace N64Recomp {

LiveGeneratorOutput::~LiveGeneratorOutput() = default;

ShimFunction::ShimFunction(recomp_func_ext_t *, uintptr_t)
    : code(nullptr), func(nullptr) {
}

ShimFunction::~ShimFunction() {
}

} // namespace N64Recomp
