cmake_minimum_required(VERSION 4.2)
project(sediment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sediment STATIC src/sediment.cpp)
target_include_directories(sediment PUBLIC include)
