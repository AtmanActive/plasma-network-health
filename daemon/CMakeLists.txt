# SPDX-License-Identifier: MIT
find_package(Threads REQUIRED)

add_executable(plasma-network-healthd
    src/main.cpp
    src/monitor.cpp
    src/pinger.cpp
    src/resolver.cpp
    src/util.cpp)

target_compile_features(plasma-network-healthd PRIVATE cxx_std_17)
target_link_libraries(plasma-network-healthd PRIVATE Threads::Threads)

# Keeps --version in step with the project version; main.cpp has a fallback so
# the sources still build without CMake.
target_compile_definitions(plasma-network-healthd PRIVATE NH_VERSION="${PROJECT_VERSION}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(plasma-network-healthd PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(STATIC_LIBSTDCXX)
    # Leaves libc as the only shared dependency, which is what makes a release
    # tarball runnable on distributions other than the one that built it.
    target_link_options(plasma-network-healthd PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS plasma-network-healthd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
