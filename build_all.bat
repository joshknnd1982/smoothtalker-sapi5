@echo off
setlocal enabledelayedexpansion

echo ============================================
echo  SmoothTalker SAPI5 -- full build
echo ============================================
echo.

set "ROOT=%~dp0"
set "BUILD_X86=%ROOT%build_x86"
set "BUILD_X64=%ROOT%build_x64"
set "OUTPUT=%ROOT%output"

if not exist "%OUTPUT%" mkdir "%OUTPUT%"

rem -- locate Visual Studio ---------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or the Build Tools.
    exit /b 1
)

set "VSINSTALLDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLDIR=%%i"
if not defined VSINSTALLDIR (
    echo ERROR: no Visual Studio C++ toolset found.
    exit /b 1
)
echo Visual Studio: %VSINSTALLDIR%

rem -- locate Inno Setup ------------------------------------------------------
rem Checked in this order because a per-user install (the first one) is what
rem you get from the default installer these days, and the Program Files
rem locations are the older machine-wide ones.
set "ISCC="
for %%p in (
    "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
    "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do if not defined ISCC if exist %%p set "ISCC=%%~p"

if defined ISCC (
    echo Inno Setup:   %ISCC%
) else (
    echo Inno Setup:   not found -- the installer will be skipped
)
echo.

rem -- build x86 --------------------------------------------------------------
echo [1/4] Building 32-bit...
cmake -A Win32 -S "%ROOT%." -B "%BUILD_X86%" >nul
if errorlevel 1 (echo ERROR: CMake configure failed for x86. & exit /b 1)
cmake --build "%BUILD_X86%" --config Release
if errorlevel 1 (echo ERROR: x86 build failed. & exit /b 1)
echo.

rem -- build x64 --------------------------------------------------------------
echo [2/4] Building 64-bit...
cmake -A x64 -S "%ROOT%." -B "%BUILD_X64%" >nul
if errorlevel 1 (echo ERROR: CMake configure failed for x64. & exit /b 1)
cmake --build "%BUILD_X64%" --config Release
if errorlevel 1 (echo ERROR: x64 build failed. & exit /b 1)
echo.

rem -- self-check -------------------------------------------------------------
echo [3/4] Checking the built engines...
rem The sample .wav each self-test writes stays beside the binaries; %OUTPUT%
rem holds the finished installer and nothing else.
rem
rem Fully qualified paths throughout, including for the executable: some
rem environments set NoDefaultCurrentDirectoryInExePath, and there a bare
rem "st_sapi_test.exe" is not found even standing in its own directory.
"%BUILD_X86%\bin\Release\st_sapi_test.exe" --direct --prefix "%BUILD_X86%\bin\Release\selftest"
if errorlevel 1 (echo ERROR: the 32-bit SAPI5 engine failed its self-test. & exit /b 1)
"%BUILD_X64%\bin\Release\st_sapi_test.exe" --direct --prefix "%BUILD_X64%\bin\Release\selftest"
if errorlevel 1 (echo ERROR: the 64-bit SAPI5 engine failed its self-test. & exit /b 1)
echo.

rem -- installer --------------------------------------------------------------
echo [4/4] Building the installer...
if not defined ISCC (
    echo Skipped: Inno Setup 6 is not installed.
    echo Built binaries are in %BUILD_X86%\bin\Release and %BUILD_X64%\bin\Release
    exit /b 0
)

"%ISCC%" /Q "%ROOT%installer\smoothtalker_sapi5.iss"
if errorlevel 1 (echo ERROR: the installer failed to compile. & exit /b 1)

echo.
echo ============================================
echo  Done.
echo    %OUTPUT%\SmoothTalkerSAPI5_Setup.exe
echo ============================================
endlocal
