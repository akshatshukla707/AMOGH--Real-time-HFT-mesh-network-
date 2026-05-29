cmake_minimum_required(VERSION 3.16)
project(hft_mesh CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build flags for maximum performance
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -funroll-loops -fno-exceptions")
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

# Add -g for profiling even in Release
add_compile_options(-Wall -Wextra -Wpedantic)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Try to find ZeroMQ (optional)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(ZMQ libzmq)
endif()

include_directories(${CMAKE_SOURCE_DIR}/cpp/include)

# Main trading engine executable
add_executable(hft_engine
    cpp/src/main.cpp
)

target_link_libraries(hft_engine
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

if(ZMQ_FOUND)
    target_link_libraries(hft_engine ${ZMQ_LIBRARIES})
    target_include_directories(hft_engine PRIVATE ${ZMQ_INCLUDE_DIRS})
    target_compile_definitions(hft_engine PRIVATE HAS_ZMQ=1)
    message(STATUS "ZeroMQ found — enabling ML bridge via ZMQ")
else()
    message(STATUS "ZeroMQ not found — using mmap for ML bridge")
endif()

# Latency benchmark utility
add_executable(latency_bench cpp/src/latency_bench.cpp)
target_link_libraries(latency_bench Threads::Threads)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX flags: ${CMAKE_CXX_FLAGS_RELEASE}")
