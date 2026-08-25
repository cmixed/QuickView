@echo off
setlocal enabledelayedexpansion

:: QuickView Adaptive LLD Linker Wrapper
:: This wrapper ensures Visual Studio environment variables (LIB/INCLUDE) are set
:: before calling lld-link, which is required for Clang-cl builds on Windows.

:: 1. Find VS installation path via vswhere
set "VS_PATH="
set "VSWHERE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "!VSWHERE_EXE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE_EXE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if not defined VS_PATH (
    echo [QuickViewLinker] Error: Could not locate Visual Studio installation. >&2
    exit /b 1
)

echo [QuickViewLinker] LIB Before = "!LIB!" >&2
:: 2. Initialize or override VS Build Environment based on target architecture
set "VCVARS_ARCH=x64"
set "TARGET_ARCH=x64"
for %%a in (%*) do (
    set "arg=%%a"
    if /I "!arg!"=="/machine:ARM64" (
        set "VCVARS_ARCH=x64_arm64"
        set "TARGET_ARCH=ARM64"
    ) else if /I "!arg!"=="-machine:ARM64" (
        set "VCVARS_ARCH=x64_arm64"
        set "TARGET_ARCH=ARM64"
    ) else if /I "!arg!"=="/machine:ARM" (
        set "VCVARS_ARCH=x64_arm"
        set "TARGET_ARCH=ARM"
    ) else if /I "!arg!"=="-machine:ARM" (
        set "VCVARS_ARCH=x64_arm"
        set "TARGET_ARCH=ARM"
    ) else if /I "!arg!"=="/machine:X86" (
        set "VCVARS_ARCH=x64_x86"
        set "TARGET_ARCH=X86"
    ) else if /I "!arg!"=="-machine:X86" (
        set "VCVARS_ARCH=x64_x86"
        set "TARGET_ARCH=X86"
    )
)

set "NEEDS_VCVARS=0"
if not defined LIB (
    set "NEEDS_VCVARS=1"
) else (
    set "TEMP_LIB=!LIB!"
    if "!TARGET_ARCH!"=="ARM64" (
        if "!TEMP_LIB:\\arm64=!"=="!TEMP_LIB!" set "NEEDS_VCVARS=1"
    ) else if "!TARGET_ARCH!"=="ARM" (
        if "!TEMP_LIB:\\arm=!"=="!TEMP_LIB!" set "NEEDS_VCVARS=1"
    ) else if "!TARGET_ARCH!"=="X86" (
        if "!TEMP_LIB:\\x86=!"=="!TEMP_LIB!" set "NEEDS_VCVARS=1"
    ) else if "!TARGET_ARCH!"=="x64" (
        if "!TEMP_LIB:\\x64=!"=="!TEMP_LIB!" set "NEEDS_VCVARS=1"
    )
)

if "!NEEDS_VCVARS!"=="1" (
    echo [QuickViewLinker] Target arch is !TARGET_ARCH!, calling vcvarsall.bat !VCVARS_ARCH!... >&2
    call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" !VCVARS_ARCH! > nul
)
echo [QuickViewLinker] LIB After = "!LIB!" >&2

:: 3. Locate lld-link.exe dynamically
set "LLVM_LINKER_EXE="

:: Priority 1: Visual Studio internal LLVM tools (guarantees bitcode version match with clang-cl)
if not defined LLVM_LINKER_EXE if exist "!VS_PATH!\VC\Tools\Llvm\x64\bin\lld-link.exe" set "LLVM_LINKER_EXE=!VS_PATH!\VC\Tools\Llvm\x64\bin\lld-link.exe"
if not defined LLVM_LINKER_EXE if exist "!VS_PATH!\VC\Tools\Llvm\bin\lld-link.exe" set "LLVM_LINKER_EXE=!VS_PATH!\VC\Tools\Llvm\bin\lld-link.exe"

:: Priority 2: Standalone LLVM in standard locations
if not defined LLVM_LINKER_EXE if exist "C:\Program Files\LLVM\bin\lld-link.exe" set "LLVM_LINKER_EXE=C:\Program Files\LLVM\bin\lld-link.exe"
if not defined LLVM_LINKER_EXE if exist "D:\Program Files\LLVM\bin\lld-link.exe" set "LLVM_LINKER_EXE=D:\Program Files\LLVM\bin\lld-link.exe"
if not defined LLVM_LINKER_EXE if exist "C:\Program Files (x86)\LLVM\bin\lld-link.exe" set "LLVM_LINKER_EXE=C:\Program Files (x86)\LLVM\bin\lld-link.exe"

:: Priority 3: System PATH
if not defined LLVM_LINKER_EXE (
    for /f "tokens=*" %%i in ('where lld-link.exe 2^>nul') do (
        set "LLVM_LINKER_EXE=%%i"
        goto :linker_found
    )
)

:linker_found
if not defined LLVM_LINKER_EXE (
    echo [QuickViewLinker] Error: lld-link.exe not found. Please install LLVM or VS LLVM components. >&2
    exit /b 1
)

:: 4. Execute the real linker with passed arguments
"!LLVM_LINKER_EXE!" %*
exit /b %ERRORLEVEL%
