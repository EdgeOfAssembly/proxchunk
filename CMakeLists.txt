cmake_minimum_required(VERSION 3.16)
project(proxchunk VERSION 1.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++23

if(NOT CMAKE_CXX_COMPILER MATCHES "g\\+\\+")
    set(CMAKE_CXX_COMPILER g++)
endif()

option(STATIC_LINK
    "Fully static link (libcurl.a plus nghttp2/brotli static-libs from local overlay)"
    OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
add_link_options(-Wl,--as-needed -Wl,-O1 -Wl,--hash-style=gnu -Wl,-z,relro)

set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -DNDEBUG -ffunction-sections -fdata-sections -march=x86-64 -mtune=generic"
    CACHE STRING "Release flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections" CACHE STRING "Release link flags" FORCE)

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

if(STATIC_LINK)
    target_compile_definitions(proxchunk PRIVATE CURL_STATICLIB)
    target_link_options(proxchunk PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(proxchunk PRIVATE
        -Wl,--start-group
        curl cares idn2 unistring rtmp ssh2 gcrypt gpg-error ssl crypto zstd z
        nghttp2 brotlidec brotlicommon
        -Wl,--end-group
        dl pthread
    )
else()
    target_link_libraries(proxchunk PRIVATE CURL::libcurl Threads::Threads)
endif()

add_executable(test_plan tests/test_plan.cpp)
target_include_directories(test_plan PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()
add_test(NAME plan COMMAND test_plan)

include(GNUInstallDirs)
install(TARGETS proxchunk DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES doc/proxchunk.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES desktop/proxchunk.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES icons/proxchunk.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
install(FILES icons/proxchunk.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/1024x1024/apps
        OPTIONAL)
