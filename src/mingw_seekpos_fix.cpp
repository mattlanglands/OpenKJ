#if defined(_WIN32) && defined(__GNUC__)
// MinGW's libstdc++ static archive is missing the out-of-line definition of
// std::basic_streambuf<char>::seekpos, which is referenced in vtables emitted
// by fmt/spdlog. This explicit instantiation forces the compiler to generate
// all member function definitions (including seekpos) in this translation unit.
#include <streambuf>
template class std::basic_streambuf<char, std::char_traits<char>>;
#endif
