cmake_minimum_required(VERSION 3.16)
project(proxchunk VERSION 1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++23

if(NOT CMAKE_CXX_COMPILER MATCHES "g\\+\\+")
    set(CMAKE_CXX_COMPILER g++)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
add_link_options(-Wl,--as-needed -Wl,-O1 -Wl,--hash-style=gnu -Wl,-z,relro)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(proxchunk src/proxchunk.cpp)
target_include_directories(proxchunk PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    /usr/local/include
)
target_compile_definitions(proxchunk PRIVATE PROXCHUNK_VERSION="${PROJECT_VERSION}")
target_link_libraries(proxchunk PRIVATE CURL::libcurl Threads::Threads)

add_executable(test_plan tests/test_plan.cpp)
target_include_directories(test_plan PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_plan PRIVATE Threads::Threads)

enable_testing()
add_test(NAME plan COMMAND test_plan)

install(TARGETS proxchunk DESTINATION bin)
