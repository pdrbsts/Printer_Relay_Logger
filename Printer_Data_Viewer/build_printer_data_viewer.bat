@echo off
echo Building printer_data_viewer (32-bit, C++17)...

REM Set up the environment for MSVC (you may need to adjust the path)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"

REM Compile resources
REM rc.exe resources.rc

REM Compile the program for Release (Optimized for Speed and Size)
REM Link against necessary Windows libraries: user32.lib, gdi32.lib, comdlg32.lib (gdiplus.lib and comctl32.lib are handled by #pragma in code)
cl.exe /EHsc /std:c++17 /O2 /GL /MD /D NDEBUG printer_data_viewer.cpp /FePrinter_Data_Viewer.exe /link /LTCG /OPT:REF /OPT:ICF

echo Build completed successfully!
pause
