@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist dist mkdir dist
cl /nologo /std:c++17 /O1 /Oi /GS /GR- /Zl /W4 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo:build\main.obj src\main.cpp /link /OUT:dist\DesktopToggle.exe /SUBSYSTEM:WINDOWS /MACHINE:X64 /ENTRY:EntryPoint /NODEFAULTLIB /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib uuid.lib BufferOverflowU.lib libcmt.lib /MAP:build\DesktopToggle.map
exit /b %errorlevel%
