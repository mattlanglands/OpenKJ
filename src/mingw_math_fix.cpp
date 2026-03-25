#if defined(_WIN32) && defined(__GNUC__)
// Fix MinGW 32-bit pseudo-reloc overflow for pow() and ceil().
// {fmt} (used by spdlog) calls these without __declspec(dllimport), causing the
// linker to emit 32-bit pseudo-reloc entries that overflow at runtime on 64-bit
// Windows because msvcrt.dll loads > 2GB from the executable image.
//
// Providing strong local definitions here causes all direct pow()/ceil() calls
// to resolve to these functions in .text (within 2GB of every caller), and the
// linker never generates pseudo-relocs for them. Each wrapper calls through the
// IAT pointer (__imp_pow / __imp_ceil), which is also in the local image.
//
// CMakeLists must pass -Wl,--allow-multiple-definition so the linker accepts
// these alongside the weak import stubs in libmsvcrt.a.
extern "C" {
    extern double (*const __imp_pow)(double, double);
    extern double (*const __imp_ceil)(double);

    double pow(double x, double y) { return (*__imp_pow)(x, y); }
    double ceil(double x)          { return (*__imp_ceil)(x); }
}
#endif
