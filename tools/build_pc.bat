@echo off
rem Builds the native Windows version (pc/) into build\win64 with clang-cl from the VS 2022 Build Tools, then copies
rem boz64.exe into the game folder (cod bo mobile\BOZ64 PC, set up by tools\setup_pc_data.ps1) when it exists.
rem usage: tools\build_pc.bat [target]
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SDK=%LOCALAPPDATA%\Android\Sdk
set CMAKE=%SDK%\cmake\3.31.6\bin\cmake.exe
set NINJA=%SDK%\cmake\3.31.6\bin\ninja.exe
set LLVM=%VS%\VC\Tools\Llvm\x64\bin
cd /d "%~dp0.."
if not exist build\win64\build.ninja (
    "%CMAKE%" -G Ninja -S pc -B build\win64 -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_C_COMPILER="%LLVM%\clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%\clang-cl.exe" ^
        -DCMAKE_LINKER="%LLVM%\lld-link.exe" -DCMAKE_BUILD_TYPE=Release > build-pc-configure.log 2>&1 || (type build-pc-configure.log & exit /b 1)
)
if "%~1"=="" (
    "%CMAKE%" --build build\win64 -j 14 || exit /b 1
) else (
    "%CMAKE%" --build build\win64 -j 14 --target %1 || exit /b 1
)
set GAME=%~dp0..\..\..\BOZ64 PC
if exist "%GAME%\data\" if exist build\win64\boz64.exe (
    copy /y build\win64\boz64.exe "%GAME%\boz64.exe" >nul || (echo could not update "%GAME%\boz64.exe": is the game running? & exit /b 1)
    copy /y "pc\dist\*.txt" "%GAME%\" >nul
)
