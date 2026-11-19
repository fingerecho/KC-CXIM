@echo off
rem ===========================================================================
rem build_msvc.bat - build the IOCP friend-lifecycle stress client
rem
rem Works even when you launch it from a plain cmd window: if the MSVC
rem environment is not loaded (INCLUDE empty -> C1083 'windows.h' not found),
rem the script locates VS via vswhere and calls vcvarsall.bat x64 itself.
rem
rem This batch file MUST stay ASCII-only with CRLF endings. cmd.exe reads
rem batch files in the OEM code page; UTF-8 Chinese comments plus LF make the
rem parser lose line boundaries and execute fragments of comments as commands.
rem
rem Usage: build_msvc.bat [rapidjson_dir] [msg_codec_dir] [out_dir] [obj_dir]
rem   out_dir : where the .exe goes, created if missing
rem   obj_dir : where intermediate .obj files go, created if missing
rem                    (the one where rapidjson\document.h resolves as
rem                     <rapidjson_dir>\rapidjson\document.h)
rem   msg_codec_dir  : the dir holding message.h and message_codec.h
rem ===========================================================================

setlocal

set "RJSON=%~1"
if "%RJSON%"=="" set "RJSON=..\third-party\RapidJSON"
set "MSGDIR=%~2"
if "%MSGDIR%"=="" set "MSGDIR=..\step1_tcp_echo"
set "OUTDIR=%~3"
if "%OUTDIR%"=="" set "OUTDIR=..\build\bin\Release"
set "OBJDIR=%~4"
if "%OBJDIR%"=="" set "OBJDIR=..\build\obj\stress03_iocp"

call :ensure_msvc_env
if errorlevel 1 exit /b 1

rem NOTE: /Fe: and /Fo: do NOT create missing directories. Without this mkdir
rem the link fails with LNK1104 (cannot open output file, system error 3).
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OUTDIR%\*" goto :bad_out
if not exist "%OBJDIR%\*" goto :bad_out
echo [INFO] out=%OUTDIR%  obj=%OBJDIR%

echo [INFO] include dirs: .  %RJSON%  %MSGDIR%
echo [INFO] link: ws2_32.lib (import lib from the Windows SDK, auto-found via LIB)

rem QUOTING RULE: never end a quoted path with a single backslash. In the MSVC
rem command line, backslash-quote is an escaped quote, so /Fo"dir\" leaves the
rem quote open and swallows every later option into /Fo (symptom: fatal error
rem C1083 on a "file name" that contains the rest of the command line).
rem One source file here, so name the .obj explicitly - no trailing backslash.
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 test_stress_03_iocp_opt.cpp /I"." /I"%RJSON%" /I"%MSGDIR%" /Fo"%OBJDIR%\test_stress_03_iocp.obj" /link ws2_32.lib /Fe:"%OUTDIR%\test_stress_03_iocp.exe"


move ./test_stress_03_iocp.exe ../build/bin/Release/

if errorlevel 1 goto :build_failed

echo [OK] built %OUTDIR%\test_stress_03_iocp.exe   (obj: %OBJDIR%)
echo [OK] smoke:  "%OUTDIR%\test_stress_03_iocp.exe" --host 127.0.0.1 --friends 20
echo [OK] load:   "%OUTDIR%\test_stress_03_iocp.exe" --host 127.0.0.1 --friends 5000 --ramp-up 300
exit /b 0

rem --- auto-load MSVC env when this script is run outside a dev prompt ---
:ensure_msvc_env
if defined INCLUDE goto :env_ok
echo [INFO] INCLUDE is empty, loading MSVC x64 environment automatically...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :probe_vs

echo [ERR] vswhere.exe not found at "%VSWHERE%"
echo [ERR] Visual Studio 2022 C++ toolchain appears not installed. Install it,
echo [ERR] then in VS Installer check: Desktop development with C++ workload,
echo [ERR] and under Individual components: Windows 11 SDK.
exit /b 1

:probe_vs
set "VSPATH="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%I"
if not defined VSPATH goto :no_vs
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto :no_vs
call "%VCVARS%" x64
if defined INCLUDE goto :env_ok
echo [ERR] vcvarsall ran but INCLUDE still empty - Windows SDK likely missing.
echo [ERR] VS Installer - Modify - check the Windows 11 SDK component.
exit /b 1

:no_vs
echo [ERR] No Visual Studio with the C++ toolset found via vswhere.
echo [ERR] Alternative: start "x64 Native Tools Command Prompt for VS 2022"
echo [ERR] from the Start menu, cd to this folder, then rerun this script.
exit /b 1

:env_ok
goto :eof

:bad_out
echo [ERR] Cannot prepare out dir "%OUTDIR%" or obj dir "%OBJDIR%".
echo [ERR] Cause: parent path missing/not writable, or a file with the same
echo [ERR] name already exists there. Pass a valid out_dir as the 3rd arg.
exit /b 1


:build_failed
echo [ERR] Build failed. The notes below are a per-error checklist -
echo [ERR]   only act on the one that matches your actual error text.
echo [ERR] C1083 windows.h : MSVC env not loaded. Start "x64 Native Tools
echo [ERR]   Command Prompt for VS 2022" (or newer) and run this script there.
echo [ERR] C1083 rapidjson/document.h : 1st arg wrong. Pass the directory that
echo [ERR]   CONTAINS the rapidjson folder, not the rapidjson folder itself.
echo [ERR] C1083 message.h : 2nd arg wrong. Pass the dir holding message.h and
echo [ERR]   message_codec.h.
echo [ERR] LNK1104 ws2_32.lib : Windows SDK lib dir missing from LIB. Install
echo [ERR]   the Windows SDK, or use the x64 Native Tools prompt.
echo [ERR] C4267 in message_codec.h : that is your project header, not this
echo [ERR]   file. Harmless warning; silence with /wd4267 if it is noise.
exit /b 1
