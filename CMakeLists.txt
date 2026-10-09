cmake_minimum_required(VERSION 4.2)
project(sediment_kv)

set(CMAKE_CXX_STANDARD 20)

add_library(sediment_kv STATIC library.cpp)
