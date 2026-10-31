cmake_minimum_required(VERSION 3.20)

project(QuantLOB
    VERSION 1.0.0
    DESCRIPTION "Sub-microsecond Lock-Free C++20 Limit Order Book Architecture"
    LANGUAGES CXX
)

# Enforce C++20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strictly default to Release mode
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Strictly Release mode for HFT sub-microsecond performance" FORCE)
endif()

# Hot-path optimization flags for GCC/Clang targeting C++20 Release mode
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
        -O3
        -march=native
        -flto
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
    )
    add_link_options(-flto)
elseif(MSVC)
    add_compile_options(
        /O2
        /Oi
        /Ot
        /GL
        /W4
        /permissive-
    )
    add_link_options(/LTCG)
endif()

# Limit Order Book Benchmark Executable
add_executable(lob_benchmark
    src/main.cpp
    src/order_book.cpp
)

target_include_directories(lob_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Threading support
find_package(Threads REQUIRED)
target_link_libraries(lob_benchmark PRIVATE Threads::Threads)

