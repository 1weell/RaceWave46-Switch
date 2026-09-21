#include <switch.h>

#include <cinttypes>
#include <cstdio>

extern "C" int main(int argc, char** argv);
extern "C" void wr64_switch_trace(const char* message);

// libnx switches to this stack before invoking the user exception handler.
// Keep it independent from the potentially corrupted thread stack.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern "C" void __libnx_exception_handler(ThreadExceptionDump* context) {
    // Do not acquire the boot logger's mutex from an exception handler: the
    // interrupted thread may own it. Write the independent exception file.
    FILE* output = std::fopen("sdmc:/switch/RaceWave46/exception.log", "wb");
    if (output == nullptr) output = std::fopen("sdmc:/switch/wr64_exception.log", "wb");
    if (output == nullptr) return;

    std::setvbuf(output, nullptr, _IONBF, 0);
    std::fprintf(output, "error_desc=0x%08" PRIX32 "\n", context->error_desc);
    std::fprintf(output, "main=0x%016" PRIX64 "\n",
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&main)));
    std::fprintf(output, "pc=0x%016" PRIX64 "\n", context->pc.x);
    std::fprintf(output, "lr=0x%016" PRIX64 "\n", context->lr.x);
    std::fprintf(output, "sp=0x%016" PRIX64 "\n", context->sp.x);
    std::fprintf(output, "fp=0x%016" PRIX64 "\n", context->fp.x);
    std::fprintf(output, "far=0x%016" PRIX64 "\n", context->far.x);
    std::fprintf(output, "esr=0x%08" PRIX32 " pstate=0x%08" PRIX32 "\n",
        context->esr, context->pstate);
    for (int index = 0; index < 29; ++index) {
        std::fprintf(output, "x%d=0x%016" PRIX64 "\n", index, context->cpu_gprs[index].x);
    }
    std::fclose(output);
}
