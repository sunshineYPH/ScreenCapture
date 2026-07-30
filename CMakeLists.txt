cmake_minimum_required(VERSION 3.10)
project(ScreenCapture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find X11
find_package(X11 REQUIRED)
include_directories(${X11_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Find libpng for PNG icon decoding (libpng16 is on every Linux desktop).
# The project also has a hand-rolled PNG encoder in png_io.cpp; libpng
# is only used to *read* the small menu-icon assets.
find_path(PNG_INCLUDE_DIRS png.h)
find_library(PNG_LIBRARIES png)
if(PNG_INCLUDE_DIRS AND PNG_LIBRARIES)
    message(STATUS "Found libpng: ${PNG_LIBRARIES}")
else()
    message(WARNING "libpng not found; menu icons will be unavailable")
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/capture.cpp
    src/renderer.cpp
    src/png_io.cpp
    src/clipboard.cpp
    src/pin_window.cpp
    src/ui_helpers.cpp
    src/button_icons.cpp
)

target_link_libraries(${PROJECT_NAME}
    ${X11_LIBRARIES}
    Xrender
    ${PNG_LIBRARIES}
    z
)

if(PNG_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${PNG_INCLUDE_DIRS})
endif()

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

set(INSTALL_DIR ${CMAKE_SOURCE_DIR}/install)
install(DIRECTORY assets DESTINATION ${INSTALL_DIR})
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${INSTALL_DIR})
