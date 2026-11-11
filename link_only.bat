@echo off
setlocal
set VSDIR=D:\BuildTools2019
set QTDIR=D:\Qt\5.15.2\msvc2019_64
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=%QTDIR%\bin;%PATH%
set CL=/MP
cd /d D:\RedisDesktopManager\src
nmake /NOLOGO
if errorlevel 1 (echo LINK_STEP_FAILED & exit /b 1)
echo LINK_STEP_OK
exit /b 0
