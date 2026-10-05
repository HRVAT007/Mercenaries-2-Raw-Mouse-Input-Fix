@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MT /EHsc /W3 /DNDEBUG /D_WIN32_WINNT=0x0601 ^
   /Fobuild\ /Febuild\Mercs2Fix.asi ^
   src\Mercs2Fix.cpp ^
   /link /INCREMENTAL:NO /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib advapi32.lib shell32.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo.
echo Built: build\Mercs2Fix.asi
endlocal
