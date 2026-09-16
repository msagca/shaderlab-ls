@echo off
rem Configures and builds shaderlab-ls with MSVC + Ninja. Usage: build.cmd [Release|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL (
  echo Visual Studio with the C++ toolset was not found.
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1
cmake --build "%~dp0build" || exit /b 1
