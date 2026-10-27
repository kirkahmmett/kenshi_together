@echo off
REM Build KenshiTogether.dll with the legacy v100 (VC++ 2010) x64 toolchain on a
REM machine that has only "Windows SDK 7.1 + VC2010 SP1 compiler update" (no full
REM VS2010). We hand MSBuild a complete PATH/INCLUDE/LIB and UseEnv=true so it does
REM not rely on VS2010 registry/SDK auto-detection.
REM
REM Prereqs (see resources/BUILD_SETUP.md):
REM   - VC++ 2010 (v100) x64 compiler  (SDK 7.1 + KB2519277)
REM   - VS2022 Build Tools (for MSBuild.exe)
REM   - third_party/KenshiLib_deps (deps + Boost) and env vars set
REM   - a KenshiLib SDK that compiles this tree (v0.4.1; see CLAUDE.md "KenshiLib
REM     surumu"): KenshiLib\Include + KenshiLib\Libraries, picked in this order:
REM       1. %KENSHILIB_SDK%          (env var, if set)
REM       2. third_party\KenshiLib_sdk (git-ignored, if present)
REM       3. third_party\KenshiLib_deps (the deps repo's own KenshiLib; v0.4.0
REM          at its master does NOT compile this tree)
REM     Boost always comes from third_party\KenshiLib_deps\boost_1_60_0.
REM   - third_party/enet/enet patched for C89 (scripts/apply_enet_patch is implicit;
REM     see third_party/enet/patches/0001-enet-c89-for-loops.patch)
setlocal

REM Build configuration (Phase 1 build separation). Default = Harness, the
REM optimized TEST build that INCLUDES the scenario runner - this is what the
REM regression/manual pipeline needs. Pass "Release" to produce the shipped
REM player DLL (no scenario code); "Debug" for a dev build.
REM   Usage:  scripts\build_plugin.cmd [Harness|Release|Debug]
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"

REM Repo root = parent of this script's folder.
set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"
set "KL=%REPO%\third_party\KenshiLib_deps"
set "ENET=%REPO%\third_party\enet\enet\include"

REM KenshiLib headers + import libs (see the prereq note above for the order).
set "KLSDK=%KL%"
if exist "%REPO%\third_party\KenshiLib_sdk\KenshiLib\Include" set "KLSDK=%REPO%\third_party\KenshiLib_sdk"
if defined KENSHILIB_SDK set "KLSDK=%KENSHILIB_SDK%"
if not exist "%KLSDK%\KenshiLib\Include\kenshi" (
    echo ERROR: no KenshiLib headers at "%KLSDK%\KenshiLib\Include"
    exit /b 1
)

REM Locate MSBuild via vswhere (falls back to a common path).
set "MSBUILD="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2^>nul`) do set "MSBUILD=%%i"
if not defined MSBUILD set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

REM x64 native toolchain on PATH so cl.exe finds its sibling DLLs (mspdb100, etc).
set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"

REM Headers: VC10 CRT + Win SDK 7.1 + vc10_compat ammintrin.h shim + our deps.
REM ...\Include\ogre is needed because the vendored ogre math headers include
REM each other by bare name ("OgreVector3.h"); vc10_compat also shims the
REM missing OgreConfig.h/OgrePlatformInformation.h that chain pulls in.
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat;%KLSDK%\KenshiLib\Include;%KLSDK%\KenshiLib\Include\ogre;%KL%\boost_1_60_0;%ENET%"

REM Libs: VC10 x64 CRT + Win SDK 7.1 x64 + KenshiLib (kenshilib.lib, OgreMain_x64.lib).
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KLSDK%\KenshiLib\Libraries"

echo === Building KenshiTogether.dll (%CONFIG%^|x64, v100) ===
echo KenshiLib SDK: %KLSDK%
where cl.exe

REM UseEnv=true: use the INCLUDE/LIB/PATH above instead of registry-derived paths.
REM TrackFileAccess=false: avoid Tracker.exe TRK0002 under redirected shells.
"%MSBUILD%" "%REPO%\src\plugin\KenshiCoop.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal

endlocal
