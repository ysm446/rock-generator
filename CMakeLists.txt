cmake_minimum_required(VERSION 3.25)

project(RockGenerator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

add_executable(rock_generator WIN32
    src/main.cpp
    src/node_graph.cpp
    src/obj_exporter.cpp
    src/sdf_preview.cpp
)

target_link_libraries(rock_generator PRIVATE
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    d3d12
    dxgi
    d3dcompiler
)

target_compile_definitions(rock_generator PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(rock_generator PRIVATE /W4 /permissive-)
endif()
