cmake_minimum_required(VERSION 3.20)

project(QuantLOB
    VERSION 1.0.0
    DESCRIPTION "Sub-microsecond Lock-Free C++20 Limit Order Book & Quantitative Matching Engine"
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
        -Wno-interference-size
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

# Core LOB Engine Static Library
add_library(lob_engine STATIC
    src/order_book.cpp
    src/feed_handler.cpp
    src/trading_engine.cpp
)

target_include_directories(lob_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(lob_engine PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(lob_engine PUBLIC rt)
endif()


# Benchmark Suite Executable
add_executable(lob_benchmark
    src/main.cpp
)
target_link_libraries(lob_benchmark PRIVATE lob_engine)

# Invariant Unit Test Executable
add_executable(lob_tests
    tests/test_order_book.cpp
)
target_link_libraries(lob_tests PRIVATE lob_engine)

# Pre-Trade Risk & Execution Gateway Invariant Tests
add_executable(lob_risk_tests
    tests/test_risk_and_execution.cpp
)
target_link_libraries(lob_risk_tests PRIVATE lob_engine)

# MoldUDP64 Multicast Protocol & TCA Invariant Tests
add_executable(lob_network_tca_tests
    tests/test_network_and_tca.cpp
)
target_link_libraries(lob_network_tca_tests PRIVATE lob_engine)

# Deterministic PCAP Replay, AF_XDP & NUMA Invariant Tests
add_executable(lob_pcap_tests
    tests/test_pcap_and_numa.cpp
)
target_link_libraries(lob_pcap_tests PRIVATE lob_engine)

# Shared C-ABI Library for Python Quant Research SDK
add_library(quant_lob SHARED
    src/order_book.cpp
    src/feed_handler.cpp
    src/quant_lob_c_api.cpp
)
target_include_directories(quant_lob
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(quant_lob PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(quant_lob PUBLIC rt)
endif()

# Production 4-Core Pipelined Trading Engine Service
add_executable(lob_engine_service
    src/trading_engine_main.cpp
)
target_link_libraries(lob_engine_service PRIVATE lob_engine)

# Standalone High-Speed Binary PCAP Replay CLI Tool
add_executable(lob_pcap_replay
    tools/pcap_replay_tool.cpp
)
target_link_libraries(lob_pcap_replay PRIVATE lob_engine)



