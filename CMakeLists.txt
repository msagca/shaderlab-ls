cmake_minimum_required(VERSION 3.25)

project(shaderlab_ls VERSION 0.2.0 LANGUAGES CXX)

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

# DXC's API headers, pinned to a release. Only the headers: the library is loaded at run time, from the Unity editor,
# the dxcPath setting or the system, so a missing DXC costs HLSL diagnostics and not the build.
set(DXC_TAG v1.9.2607)
set(DXC_INCLUDE ${CMAKE_BINARY_DIR}/_deps/dxc/include)
function(fetch_dxc_header name hash)
  file(DOWNLOAD
    https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/${DXC_TAG}/include/dxc/${name}
    ${DXC_INCLUDE}/dxc/${name}
    EXPECTED_HASH SHA256=${hash}
    TLS_VERIFY ON)
endfunction()
fetch_dxc_header(dxcapi.h a8d409642ded485ea4fd4c055f1199a13538f0393755c2d958ade281afed4ca0)
fetch_dxc_header(WinAdapter.h f5688a1408a8de8c0c35176bc900f21d7679d492215da94da4ab643cb66867f4)

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
  src/format/style.cpp
  src/compiler/dxc.cpp
  src/compiler/fxc.cpp
  src/analysis/analysis.cpp
  src/analysis/hlsl_check.cpp
)

# The executable goes straight into build/ whatever the generator: a multi-config one, such as Visual Studio, would otherwise put it in build/Release/,
# and the plugin, the workflows and the tests all look for it in build/.
set_target_properties(shaderlab-ls PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}>)

target_include_directories(shaderlab-ls PRIVATE src)
target_include_directories(shaderlab-ls SYSTEM PRIVATE ${DXC_INCLUDE})
target_link_libraries(shaderlab-ls PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_definitions(shaderlab-ls PRIVATE SHADERLAB_LS_VERSION="${PROJECT_VERSION}")

if(WIN32)
  # FXC (d3dcompiler_47.dll) is Windows only; elsewhere HLSL is compiled with DXC, when there is one.
  target_compile_definitions(shaderlab-ls PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(shaderlab-ls PRIVATE Threads::Threads)
endif()

if(MSVC)
  target_compile_options(shaderlab-ls PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
  target_compile_options(shaderlab-ls PRIVATE -Wall -Wextra)
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  set(tests ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME lsp COMMAND Python3::Interpreter ${tests}/run_tests.py $<TARGET_FILE:shaderlab-ls>)
  add_test(NAME fuzz COMMAND Python3::Interpreter ${tests}/fuzz.py $<TARGET_FILE:shaderlab-ls> 50)
  set_tests_properties(fuzz PROPERTIES ENVIRONMENT FUZZ_COMPILE=1 LABELS slow)
  # Skipped rather than failed where there is no Neovim 0.12+ to drive. Run alone: its fake builds wait for fixed
  # intervals to be observed mid-run, which the other suites' compilers could stretch.
  add_test(NAME plugin COMMAND Python3::Interpreter ${tests}/run_plugin_tests.py $<TARGET_FILE:shaderlab-ls>)
  set_tests_properties(plugin PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL ON)
endif()
