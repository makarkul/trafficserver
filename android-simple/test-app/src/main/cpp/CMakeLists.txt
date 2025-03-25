cmake_minimum_required(VERSION 3.22.1)
project(trafficserver_test)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Create TrafficServer library
add_library(trafficserver INTERFACE)
target_include_directories(trafficserver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Create our JNI library
add_library(trafficserver_test SHARED
    ProxyService.cpp
    TSBridge.cpp
    ts/HTTP.cpp
)

# Find required libraries
find_library(
    log-lib
    log
)

# Link libraries
target_link_libraries(trafficserver_test
    trafficserver
    ${log-lib}
    android
)

# Include directories
target_include_directories(
    trafficserver_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Compiler definitions
add_compile_definitions(
    STANDALONE_IOCORE
    USE_MEMORY_POOLS
    HTTP_CACHE
    HAVE_POSIX_CAPS
)
