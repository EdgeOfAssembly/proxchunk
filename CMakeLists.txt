cmake_minimum_required(VERSION 3.20)
project(proxchunk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(proxchunk src/main.cpp)
target_link_libraries(proxchunk PRIVATE CURL::libcurl pthread)

install(TARGETS proxchunk DESTINATION bin)
