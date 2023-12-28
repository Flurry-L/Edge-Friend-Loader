cmake_minimum_required(VERSION 3.15) # 指定CMake的最低版本要求

project(MyProject) # 设置这个项目的名字

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(glm)
# 包含头文件目录
include_directories(include)

# 自动将当前目录的源文件和头文件加入
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp")

# 如果你想要将hlsl文件添加到你的项目中，可以使用以下命令
file(GLOB_RECURSE HLSL_FILES "hlsl/*.hlsl")

# 添加可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${HLSL_FILES} "include/dx.h" "src/dx.cpp" "include/DXSampleHelper.h")

# 根据你的需求链接库
# target_link_libraries(${PROJECT_NAME} your_libraries_here)
target_link_libraries(${PROJECT_NAME} PRIVATE d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib glm::glm)