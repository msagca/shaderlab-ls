cmake_minimum_required(VERSION 3.25)

project(shaderlab_ls VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "shaderlab-ls targets Windows only: it uses FXC (d3dcompiler_47.dll).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT so the server is a single self-contained exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(shaderlab-ls
  src/main.cpp
  src/common/text.cpp
  src/common/util.cpp
  src/lsp/transport.cpp
  src/lsp/server.cpp
  src/lsp/features.cpp
  src/shaderlab/lexer.cpp
  src/shaderlab/parser.cpp
  src/shaderlab/reference.cpp
  src/hlsl/scanner.cpp
  src/hlsl/builtins.cpp
  src/unity/project.cpp
  src/format/formatter.cpp
  src/fxc/fxc.cpp
  src/analysis/analysis.cpp
  src/analysis/hlsl_check.cpp
)

target_include_directories(shaderlab-ls PRIVATE src)
target_link_libraries(shaderlab-ls PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(shaderlab-ls PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  SHADERLAB_LS_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
  target_compile_options(shaderlab-ls PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME lsp
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:shaderlab-ls>)
endif()
