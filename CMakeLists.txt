cmake_minimum_required(VERSION 3.22)
project(AoSCADStudio VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
find_package(OpenCASCADE REQUIRED)

qt_add_executable(aoscad-studio WIN32
    native/src/main.cpp
    native/src/MainWindow.cpp
    native/src/MainWindow.hpp
    native/src/CadModel.cpp
    native/src/CadModel.hpp
    native/src/CadViewport.cpp
    native/src/CadViewport.hpp
)

target_include_directories(aoscad-studio PRIVATE
    native/src
    ${OpenCASCADE_INCLUDE_DIR}
)

if(OpenCASCADE_LIBRARY_DIR)
    target_link_directories(aoscad-studio PRIVATE ${OpenCASCADE_LIBRARY_DIR})
endif()
target_link_libraries(aoscad-studio PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ${OpenCASCADE_LIBRARIES}
)

if(MSVC)
    target_compile_options(aoscad-studio PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(aoscad-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET aoscad-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:aoscad-studio>/assets"
)

install(TARGETS aoscad-studio RUNTIME DESTINATION .)
install(DIRECTORY assets DESTINATION .)

if(OpenCASCADE_RESOURCE_DIR AND EXISTS "${OpenCASCADE_RESOURCE_DIR}")
    add_custom_command(TARGET aoscad-studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${OpenCASCADE_RESOURCE_DIR}"
                "$<TARGET_FILE_DIR:aoscad-studio>/opencascade-resources"
    )
    install(DIRECTORY "${OpenCASCADE_RESOURCE_DIR}/" DESTINATION opencascade-resources)
endif()
