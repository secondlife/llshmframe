@echo off

set N=2

if "%~1"=="" (
    echo No argument was provided - using default consumer count
) else (
    echo Argument found! You passed: %~1
    set N=%~1
)

for /L %%i in (1, 1, %N%) do (
    echo Starting consunmer: %%i
    start build\release\llshmframe_multiview_consumer.exe
)
