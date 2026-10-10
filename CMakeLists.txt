cmake_minimum_required(VERSION 3.25)

project(hotpath VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOTPATH_BUILD_TESTS "Build unit tests" ON)
option(HOTPATH_BUILD_APPS "Build the pipeline executables" ON)
option(HOTPATH_WERROR "Treat warnings as errors" ON)
option(HOTPATH_NATIVE "Tune for the build machine" OFF)
set(HOTPATH_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HotpathFlags)

find_package(Threads REQUIRED)

add_library(hotpath INTERFACE)
add_library(hotpath::hotpath ALIAS hotpath)
target_include_directories(hotpath INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hotpath INTERFACE cxx_std_20)
target_link_libraries(hotpath INTERFACE Threads::Threads)

if(HOTPATH_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(HOTPATH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
