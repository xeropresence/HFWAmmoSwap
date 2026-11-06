@echo off
rem Builds build\HFWAmmoSwap.dll, a Native Loader mod, and the release zip build\HFWAmmoSwap.zip
rem (contains HFWAmmoSwap\ to drop into <game>\nativemods\).
rem Needs Visual Studio (or Build Tools) with the C++ x64 tools. AMMOSWAP_VERSION, if set, is
rem written into the DLL's log line; it defaults to "dev".
setlocal
set ROOT=%~dp0
if "%AMMOSWAP_VERSION%"=="" set AMMOSWAP_VERSION=dev

if not exist "%ROOT%third_party\minhook\src\hook.c" (
  echo MinHook is missing: run "git submodule update --init" first.
  exit /b 1
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
  echo vswhere.exe not found: install Visual Studio or the Build Tools with the C++ workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
  echo No Visual Studio installation with the C++ x64 tools found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if exist "%ROOT%build\obj" rmdir /s /q "%ROOT%build\obj"
mkdir "%ROOT%build\obj"

set MH=%ROOT%third_party\minhook
cl /nologo /c /O2 /MT /W3 /Fo"%ROOT%build\obj\\" ^
  "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1

cl /nologo /c /O2 /MT /W4 /EHsc /std:c++20 /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DAMMOSWAP_VERSION=\"%AMMOSWAP_VERSION%\" ^
  /I"%MH%\include" /Fo"%ROOT%build\obj\\" ^
  "%ROOT%src\main.cpp" "%ROOT%src\hooks.cpp" "%ROOT%src\ammo.cpp" "%ROOT%src\bonus.cpp" "%ROOT%src\menu.cpp" "%ROOT%src\log.cpp" "%ROOT%src\crashlog.cpp" "%ROOT%src\signatures.cpp" || exit /b 1
link /nologo /DLL /OUT:"%ROOT%build\HFWAmmoSwap.dll" /PDB:"%ROOT%build\HFWAmmoSwap.pdb" /DEBUG:FULL /OPT:REF /OPT:ICF ^
  "%ROOT%build\obj\*.obj" user32.lib kernel32.lib psapi.lib || exit /b 1

set PKG=%ROOT%build\package\HFWAmmoSwap
if exist "%ROOT%build\package" rmdir /s /q "%ROOT%build\package"
mkdir "%PKG%"
copy /y "%ROOT%build\HFWAmmoSwap.dll" "%PKG%\" >nul || exit /b 1
copy /y "%ROOT%package\README.txt" "%PKG%\" >nul || exit /b 1
if exist "%ROOT%build\HFWAmmoSwap.zip" del "%ROOT%build\HFWAmmoSwap.zip"
tar -a -c -f "%ROOT%build\HFWAmmoSwap.zip" -C "%ROOT%build\package" HFWAmmoSwap || exit /b 1
echo Built HFW Ammo Swap %AMMOSWAP_VERSION%: %ROOT%build\HFWAmmoSwap.dll and %ROOT%build\HFWAmmoSwap.zip
