// Smoke test for dynarmic's x86-64 backend on Windows: runs a few ARM instructions.
#include <cstdio>
#include <cstring>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

using namespace Dynarmic;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Env final : A32::UserCallbacks {
    std::vector<unsigned char> mem = std::vector<unsigned char>(0x10000);
    A32::Jit* jit = nullptr;
    template <class T> T rd(u32 a) { T v; memcpy(&v, &mem[a & 0xffff], sizeof v); return v; }
    template <class T> void wr(u32 a, T v) { memcpy(&mem[a & 0xffff], &v, sizeof v); }
    std::optional<u32> MemoryReadCode(u32 a) override { return rd<u32>(a); }
    u8 MemoryRead8(u32 a) override { return rd<u8>(a); }
    u16 MemoryRead16(u32 a) override { return rd<u16>(a); }
    u32 MemoryRead32(u32 a) override { return rd<u32>(a); }
    u64 MemoryRead64(u32 a) override { return rd<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { wr(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { wr(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { wr(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { wr(a, v); }
    void InterpreterFallback(u32, size_t) override {}
    void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
    void ExceptionRaised(u32 pc, A32::Exception) override { printf("exception at %08x\n", pc); jit->HaltExecution(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000; }
};

int main() {
    Env env;
    // mov r0, #40; add r0, r0, #2; svc #0
    const u32 code[] = {0xe3a00028, 0xe2800002, 0xef000000};
    memcpy(env.mem.data(), code, sizeof code);
    A32::UserConfig cfg;
    cfg.callbacks = &env;
    A32::Jit jit(cfg);
    env.jit = &jit;
    jit.Regs()[15] = 0;
    jit.SetCpsr(0x10);
    jit.Run();
    printf("r0 = %u (expect 42)\n", jit.Regs()[0]);
    return jit.Regs()[0] == 42 ? 0 : 1;
}
