find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
        SYSTEM
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(hotpath_unit_tests
    unit/sys_test.cpp
    unit/mem_test.cpp
    unit/ipc_test.cpp
)
target_link_libraries(hotpath_unit_tests PRIVATE hotpath::hotpath hotpath::flags GTest::gtest_main)

gtest_discover_tests(hotpath_unit_tests)
