cmake_minimum_required(VERSION 3.25)

project(RockGenerator VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

configure_file(
    ${CMAKE_SOURCE_DIR}/src/Version.h.in
    ${CMAKE_BINARY_DIR}/generated/Version.h
    @ONLY
)

add_executable(rock_generator WIN32
    src/main.cpp
    src/node_graph.cpp
    src/obj_exporter.cpp
    src/sdf_preview.cpp
    src/ui/UiTheme.cpp
)

target_link_libraries(rock_generator PRIVATE
    imgui::imgui
    nlohmann_json::nlohmann_json
    unofficial::imgui-node-editor::imgui-node-editor
    d3d12
    dxgi
    d3dcompiler
    comdlg32
)

target_include_directories(rock_generator PRIVATE
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(rock_generator PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(rock_generator PRIVATE /W4 /permissive- /utf-8)
endif()

add_custom_command(TARGET rock_generator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data
        $<TARGET_FILE_DIR:rock_generator>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:rock_generator>/shaders
    COMMENT "Copying data files..."
)
