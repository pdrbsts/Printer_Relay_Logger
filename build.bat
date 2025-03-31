@echo off
echo Building Printer_Relay_Logger (32-bit, C++17)...

REM Set up the environment for MSVC (you may need to adjust the path)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"

REM Compile resources
REM rc.exe resources.rc

REM Compile the program for Release (Optimized for Speed and Size)
cl.exe /EHsc /std:c++17 /O2 /GL /MD /D NDEBUG Printer_Relay_Logger.cpp /FePrinter_Relay_Logger.exe /link /LTCG /OPT:REF /OPT:ICF

echo Build completed successfully!
pause
