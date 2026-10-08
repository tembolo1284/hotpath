if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "hotpath targets x86-64 Linux")
endif()

if(HOTPATH_SANITIZE)
    add_compile_options(-fsanitize=${HOTPATH_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${HOTPATH_SANITIZE})
    if(HOTPATH_SANITIZE MATCHES "undefined")
        add_compile_options(-fno-sanitize=vptr)
        add_link_options(-fno-sanitize=vptr)
    endif()
endif()

add_library(hotpath_flags INTERFACE)
add_library(hotpath::flags ALIAS hotpath_flags)

target_compile_options(hotpath_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

if(HOTPATH_WERROR)
    target_compile_options(hotpath_flags INTERFACE -Werror)
endif()

if(HOTPATH_NATIVE)
    target_compile_options(hotpath_flags INTERFACE -march=native)
endif()
