# Turn a file into a C string, at build time:
#   cmake -DINPUT=keymaps/us.xkb -DOUTPUT=keymap-us.c -DNAME=sirius_keymap_us -P embed.cmake
# defines `const char NAME[]`, the file's bytes and a terminating NUL.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "/* Generated from ${INPUT} by embed.cmake. */\nconst char ${NAME}[] = {${bytes}0};\n")
