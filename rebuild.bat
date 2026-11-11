@echo off
REM ============================================================
REM  RESP.app (Redis Desktop Manager) 增量重建脚本
REM  工具�? VS2019 Build Tools 14.29 + Qt 5.15.2 msvc2019_64
REM  用法: 双击�?cmd 中执�?rebuild.bat
REM ============================================================
setlocal
set VSDIR=D:\BuildTools2019
set QTDIR=D:\Qt\5.15.2\msvc2019_64
set SRC=D:\RedisDesktopManager\src

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=%QTDIR%\bin;%PATH%
set CL=/MP
cd /d %SRC%

qmake resp.pro -spec win32-msvc "CONFIG+=release"
if errorlevel 1 goto :fail

nmake /NOLOGO
if errorlevel 1 goto :fail

echo.
echo Build OK  -^>  D:\RedisDesktopManager\bin\windows\release\resp.exe
exit /b 0

:fail
echo Build FAILED
exit /b 1

