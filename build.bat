@echo off
setlocal EnableDelayedExpansion

set VCVARS=
for %%v in (2022 2019 2017) do (
    for %%e in (Community Professional Enterprise BuildTools) do (
        set T=C:\Program Files\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvarsall.bat
        if exist "!T!" (
            set VCVARS=!T!
            goto :found
        )
    )
)
:found
if "%VCVARS%"=="" (
    echo ERROR: Visual Studio not found
    pause
    exit /b 1
)

call "%VCVARS%" x64 >nul 2>&1
cd /d "%~dp0"

echo Building resources...
rc.exe /nologo /fo app.res app.rc

echo Building KeyBeep...
cl.exe main.cpp app.res /W3 /O2 /MT /EHsc /DUNICODE /D_UNICODE /link user32.lib kernel32.lib shell32.lib winmm.lib comctl32.lib gdi32.lib advapi32.lib /SUBSYSTEM:WINDOWS /OUT:KeyBeep.exe

if exist KeyBeep.exe (
    echo === KeyBeep.exe: OK ===
    
    echo Building Setup installer...
    rc.exe /nologo /fo setup.res setup.rc
    
    cl.exe setup.cpp setup.res /W3 /O2 /MT /EHsc /DUNICODE /D_UNICODE /link user32.lib kernel32.lib shell32.lib comctl32.lib gdi32.lib advapi32.lib ole32.lib uuid.lib /SUBSYSTEM:WINDOWS /OUT:setup.exe
    
    if exist setup.exe (
        echo === setup.exe: OK ===
        echo.
        echo Outputs: KeyBeep.exe and setup.exe
    ) else (
        echo === setup.exe: FAILED ===
    )
) else (
    echo === KeyBeep.exe: FAILED ===
)

endlocal
pause
