@echo off
rem Builds MarkLite with MSYS2 UCRT64 GCC (https://www.msys2.org).
rem   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
rem
rem   build.bat              MarkLite.exe
rem   build.bat installer    MarkLite-Setup.exe
rem   build.bat test         build and run the unit tests
rem   build.bat clean        remove the build output
setlocal
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64
rem usr\bin provides the sh, mkdir and rm that the Makefile uses.
set PATH=%MSYS2_ROOT%\ucrt64\bin;%MSYS2_ROOT%\usr\bin;%PATH%
cd /d "%~dp0"
mingw32-make -j%NUMBER_OF_PROCESSORS% %*
