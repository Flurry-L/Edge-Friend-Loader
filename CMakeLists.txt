cmake_minimum_required(VERSION 3.15)

project(edgefriend LANGUAGES CXX C)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#find_package(RapidObj REQUIRED)

add_subdirectory(glm)
add_compile_definitions(NOMINMAX)
# 创建 edgefriend 库
add_library(edgefriend)

target_include_directories(edgefriend PUBLIC include)
file(GLOB_RECURSE EDGEFRIEND_SOURCES "src/*.cpp")
target_sources(edgefriend PRIVATE ${EDGEFRIEND_SOURCES}   "src/edgefriend.cpp" "include/edgefriend.h" "src/obj_io.cpp" "include/obj_io.h")

target_link_libraries(edgefriend PUBLIC glm::glm)

# 创建 edgefriend_demo 可执行文件
add_executable(edgefriend_demo)

target_include_directories(edgefriend_demo PRIVATE include)
file(GLOB_RECURSE EDGEFRIEND_DEMO_SOURCES "src/*.cpp")
target_sources(edgefriend_demo PRIVATE ${EDGEFRIEND_DEMO_SOURCES}   "src/edgefriend.cpp" "include/edgefriend.h" "src/obj_io.cpp" "include/obj_io.h")

# 链接库和依赖项
target_link_libraries(edgefriend_demo PRIVATE edgefriend d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib)