cmake_minimum_required(VERSION 3.29)
project(hosihikari-ir-frontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Clang REQUIRED CONFIG)

add_executable(hosihikari-ir-frontend src/main.cpp)
target_link_libraries(
  hosihikari-ir-frontend
  PRIVATE
    clangTooling
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangAnalysis
    clangAST
    clangBasic
)
