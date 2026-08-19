cmake_minimum_required(VERSION 3.16)
project(proxchunk VERSION 1.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++23

if(NOT CMAKE_CXX_COMPILER MATCHES "g\\+\\+")
    set(CMAKE_CXX_COMPILER g++)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
add_link_options(-Wl,--as-needed -Wl,-O1 -Wl,--hash-style=gnu -Wl,-z,relro)

# Profile: same -O3 as Release plus gprof (-pg). Use: cmake -DCMAKE_BUILD_TYPE=Profile
set(CMAKE_CXX_FLAGS_PROFILE
    "-O3 -DNDEBUG -g -pg -fno-inline -march=x86-64 -mtune=generic -fno-omit-frame-pointer"
    CACHE STRING "Flags for Profile builds" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_PROFILE "-pg" CACHE STRING "Linker flags for Profile builds" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_PROFILE "-pg" CACHE STRING "Shared linker flags for Profile builds" FORCE)

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
