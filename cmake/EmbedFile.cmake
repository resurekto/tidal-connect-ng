# cmake -DIN=<file> -DOUT=<header> -DNAME=<function> -P EmbedFile.cmake
# Writes a header with `inline std::string_view NAME()` returning the file's bytes. A byte
# array, not a string literal: MSVC caps string literals at 16 KiB.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(base "${IN}" NAME)
file(WRITE "${OUT}" "#pragma once
// Generated from ${base} by cmake/EmbedFile.cmake. Do not edit.
#include <string_view>
namespace tcng::embedded {
inline std::string_view ${NAME}() {
    static const unsigned char bytes[] = {${bytes}0x00};
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes) - 1};
}
}  // namespace tcng::embedded
")
