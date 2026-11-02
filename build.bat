@echo off
rem Builds MarkLite.exe with MSYS2 UCRT64 GCC (https://www.msys2.org).
rem   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
setlocal
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64
set PATH=%MSYS2_ROOT%\ucrt64\bin;%PATH%
cd /d "%~dp0"
mingw32-make -j%NUMBER_OF_PROCESSORS% %*
