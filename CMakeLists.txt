cmake_minimum_required(VERSION 3.16)
project(proxchunk VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)

add_executable(proxchunk src/proxchunk.cpp)
target_link_libraries(proxchunk PRIVATE CURL::libcurl pthread)

install(TARGETS proxchunk DESTINATION bin)
