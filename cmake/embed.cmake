# Generates a C++ source that exposes the bytes of INPUT as
# `const std::string_view NAMESPACE::SYMBOL`.
# Usage: cmake -DINPUT=... -DOUTPUT=... -DNAMESPACE=... -DSYMBOL=... -P embed.cmake
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"#include <string_view>\n\nnamespace ${NAMESPACE} {\n\n"
"alignas(8) static const unsigned char ${SYMBOL}_data[${size} + 1] = {\n${bytes}0x00};\n\n"
"extern const std::string_view ${SYMBOL};\n"
"const std::string_view ${SYMBOL}(reinterpret_cast<const char*>(${SYMBOL}_data), ${size});\n\n"
"}  // namespace ${NAMESPACE}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
