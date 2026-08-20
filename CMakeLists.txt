cmake_minimum_required(VERSION 3.16)
project(proxchunk VERSION 1.1 LANGUAGES CXX)

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
add_link_options(-Wl,--as-needed -Wl,-O1 -Wl,--hash-style=gnu -Wl,-z,relro -Wl,--build-id=none)

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

function(proxchunk_link_curl tgt)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        /usr/local/include
    )
    target_compile_definitions(${tgt} PRIVATE PROXCHUNK_VERSION="${PROJECT_VERSION}")
    if(STATIC_LINK)
        target_compile_definitions(${tgt} PRIVATE CURL_STATICLIB)
        target_link_options(${tgt} PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_libraries(${tgt} PRIVATE
            -Wl,--start-group
            curl cares idn2 unistring rtmp ssh2 gcrypt gpg-error ssl crypto zstd z
            nghttp2 brotlidec brotlicommon
            -Wl,--end-group
            dl pthread
        )
    else()
        target_link_libraries(${tgt} PRIVATE CURL::libcurl Threads::Threads)
    endif()
endfunction()

add_executable(proxchunk src/proxchunk.cpp src/proxy_ipc.cpp)
proxchunk_link_curl(proxchunk)

add_executable(proxchunkd src/proxchunkd.cpp src/proxy_engine.cpp src/proxy_ipc.cpp)
proxchunk_link_curl(proxchunkd)

add_executable(test_plan tests/test_plan.cpp)
target_include_directories(test_plan PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(test_repl tests/test_repl.cpp)
target_include_directories(test_repl PRIVATE ${CMAKE_SOURCE_DIR}/include)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK3 gtk+-3.0)
    pkg_check_modules(VTE vte-2.91)
endif()
if(GTK3_FOUND AND VTE_FOUND)
    add_executable(proxchunk-gui src/gui.cpp)
    target_include_directories(proxchunk-gui PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${GTK3_INCLUDE_DIRS}
        ${VTE_INCLUDE_DIRS}
    )
    target_compile_definitions(proxchunk-gui PRIVATE PROXCHUNK_VERSION="${PROJECT_VERSION}")
    target_compile_options(proxchunk-gui PRIVATE ${GTK3_CFLAGS_OTHER} ${VTE_CFLAGS_OTHER})
    target_link_directories(proxchunk-gui PRIVATE ${GTK3_LIBRARY_DIRS} ${VTE_LIBRARY_DIRS})
    target_link_libraries(proxchunk-gui PRIVATE ${VTE_LIBRARIES} ${GTK3_LIBRARIES})
    message(STATUS "proxchunk-gui: GTK ${GTK3_VERSION} VTE ${VTE_VERSION}")
else()
    message(STATUS "proxchunk-gui: skipped (need gtk+-3.0 and vte-2.91)")
endif()

enable_testing()
add_test(NAME plan COMMAND test_plan)
add_test(NAME repl COMMAND test_repl)

include(GNUInstallDirs)
install(TARGETS proxchunk proxchunkd DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET proxchunk-gui)
    install(TARGETS proxchunk-gui DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES doc/proxchunk.1 doc/proxchunkd.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES desktop/proxchunk.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES icons/proxchunk.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
install(FILES icons/proxchunk.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/1024x1024/apps
        OPTIONAL)
