cmake_minimum_required(VERSION 3.24)
project(LinkForge LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "LinkForge is a native Windows application and must be built with the Windows SDK.")
endif()

add_executable(LinkForge WIN32
    src/main.cpp
    src/download_engine.cpp
    src/download_engine.h
    app.rc
    assets/LinkForge.ico
)

set_source_files_properties(assets/LinkForge.ico PROPERTIES HEADER_FILE_ONLY TRUE)
set_property(SOURCE app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/LinkForge.ico")

target_compile_features(LinkForge PRIVATE cxx_std_20)
target_compile_definitions(LinkForge PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_include_directories(LinkForge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(LinkForge PRIVATE
    winhttp
    comctl32
    uxtheme
    shell32
    ole32
    uuid
    user32
    gdi32
)

if(MSVC)
    target_compile_options(LinkForge PRIVATE /W4 /permissive- /EHsc /utf-8)

    # app.rc embeds the explicit application manifest as resource ID 1.
    # Disable the linker's implicit manifest embedding to avoid a duplicate RT_MANIFEST.
    target_link_options(LinkForge PRIVATE /MANIFEST:NO)
endif()