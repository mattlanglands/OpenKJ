#if defined(_WIN32) && defined(__GNUC__)
// Fix MinGW 32-bit pseudo-reloc overflow for pow() and ceil().
// The {fmt} library (used by spdlog) calls these without __declspec(dllimport),
// generating 32-bit pseudo-relocations that overflow at runtime on 64-bit
// Windows because msvcrt.dll loads > 2GB from the executable image.
// CMakeLists passes -Wl,--wrap,pow -Wl,--wrap,ceil to redirect all calls to
// these wrappers, which are in .text (within 2GB of all callers) and forward
// through the import stubs to the real IAT entries.
extern "C" {
    double __real_pow(double, double);
    double __real_ceil(double);

    double __wrap_pow(double x, double y) { return __real_pow(x, y); }
    double __wrap_ceil(double x)          { return __real_ceil(x); }
}
#endif
