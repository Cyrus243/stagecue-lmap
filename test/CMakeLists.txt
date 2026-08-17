include(FetchContent)

# Reuse a googletest already provided by a parent build if present; otherwise
# fetch a pinned copy for the standalone test build.
if(NOT TARGET GTest::gtest_main)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(lmap_tests
    WireTest.cpp
    RingByTimestampTest.cpp
    ReceiverTest.cpp
    LoopbackTest.cpp
)
target_link_libraries(lmap_tests PRIVATE lmap::lmap GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lmap_tests)
