@echo off
rem Configures and builds shaderlab-ls with the CMake presets. Usage: build.cmd [Release|Debug]
rem The generator is CMake's default, Visual Studio with MSVC, unless build\ was configured with another one.
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
rem cmake --build --preset reads the presets from the current directory; it has no -S.
pushd "%~dp0" || exit /b 1

rem A build folder configured with Ninja or NMake and cl, as this script used to, needs the compiler's environment,
rem which only a Visual Studio generator finds for itself. Loaded only then, and only when no one has already.
set CACHE=build\CMakeCache.txt
if exist "%CACHE%" (
  findstr /b /c:"CMAKE_GENERATOR:INTERNAL=Visual Studio" "%CACHE%" >nul || (
    where cl >nul 2>nul || call :vcvars || exit /b 1
  )
)

cmake --preset %CONFIG% || exit /b 1
cmake --build --preset %CONFIG% || exit /b 1
exit /b 0

:vcvars
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL (
  echo build\ is configured with a generator that needs the C++ toolset's environment, and Visual Studio with the
  echo C++ toolset was not found. Delete build\ to configure it afresh with the default generator.
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %errorlevel%
