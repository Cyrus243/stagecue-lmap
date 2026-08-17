cmake_minimum_required(VERSION 3.21)
project(lmap VERSION 1.0.0 LANGUAGES CXX)

# Build tests by default only when this is the top-level project. When lmap is
# pulled in via add_subdirectory() (e.g. as a submodule of StageCue) tests stay
# off so the parent configure does not fetch googletest.
option(LMAP_BUILD_TESTS "Build the lmap test suite" ${PROJECT_IS_TOP_LEVEL})

add_library(lmap STATIC
    src/Wire.cpp
    src/Socket.cpp
    src/Sender.cpp
    src/Receiver.cpp
)
add_library(lmap::lmap ALIAS lmap)

target_include_directories(lmap PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Public headers are C++17-clean so C++17 consumers can include them; the
# library's own translation units build at C++20.
target_compile_features(lmap PUBLIC cxx_std_17)
target_compile_features(lmap PRIVATE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(lmap PUBLIC Threads::Threads)

if(LMAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
