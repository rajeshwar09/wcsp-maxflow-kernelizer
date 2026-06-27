cmake_minimum_required(VERSION 3.16)
project(wcsp_maxflow_kernelizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All headers include from the project root (e.g. "src/cpu/graph_csr.h").
add_executable(maxflow_cpu_demo apps/maxflow_cpu_demo.cpp)
target_include_directories(maxflow_cpu_demo PRIVATE ${CMAKE_SOURCE_DIR})