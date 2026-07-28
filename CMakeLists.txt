cmake_minimum_required(VERSION 3.10)
project(ScreenCapture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find X11
find_package(X11 REQUIRED)
include_directories(${X11_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Try to find FreeType
set(FREETYPE_INCLUDE_DIRS "/usr/include/freetype2")
set(FREETYPE_LIBRARIES "")
find_library(FREETYPE_LIB freetype)
if(FREETYPE_LIB)
    set(FREETYPE_LIBRARIES ${FREETYPE_LIB})
    message(STATUS "Found FreeType: ${FREETYPE_LIB}")
else()
    message(WARNING "FreeType not found, text rendering may be limited")
endif()

add_executable(screen_capture
    src/main.cpp
    src/capture.cpp
    src/renderer.cpp
    src/png_io.cpp
    src/clipboard.cpp
    src/pin_window.cpp
    src/ui_helpers.cpp
)

target_link_libraries(screen_capture
    ${X11_LIBRARIES}
    #${FREETYPE_LIBRARIES}
    /lib/x86_64-linux-gnu/libfreetype.so.6
    z
)

if(FREETYPE_LIB)
    target_include_directories(screen_capture PRIVATE ${FREETYPE_INCLUDE_DIRS})
endif()

target_compile_options(screen_capture PRIVATE -Wall -Wextra)