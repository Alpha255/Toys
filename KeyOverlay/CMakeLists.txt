cmake_minimum_required(VERSION 3.16)

project(KeyOverlay
    VERSION 1.0.0
    DESCRIPTION "Always-on-top click-through overlay that shows what you are typing"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(KeyOverlay WIN32
    src/main.cpp
    src/config.cpp
    src/renderer.cpp
    src/overlay.cpp
    src/input_hook.cpp
)

target_include_directories(KeyOverlay PRIVATE src)

target_compile_definitions(KeyOverlay PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(KeyOverlay PRIVATE
    gdiplus
    user32
    gdi32
    shell32
    ole32
    comctl32
)

if (MSVC)
    target_compile_options(KeyOverlay PRIVATE /W4 /utf-8 /permissive- /EHsc)
    # Static CRT keeps the toy a single self-contained exe.
    set_property(TARGET KeyOverlay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(KeyOverlay PRIVATE /SUBSYSTEM:WINDOWS)
else()
    target_compile_options(KeyOverlay PRIVATE -Wall -Wextra)
    target_link_options(KeyOverlay PRIVATE -municode -mwindows -static)
endif()

# Copy the generated ini template next to the exe on first build (optional convenience).
add_custom_command(TARGET KeyOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Built $<TARGET_FILE:KeyOverlay>")
