# libsirius: the C++ system library. Also built by tests/host_harness, for
# the build machine.
add_library(sirius STATIC src/system.cpp)
target_include_directories(sirius PUBLIC include)
target_compile_options(sirius PRIVATE -O2 -Wall -Wextra)
