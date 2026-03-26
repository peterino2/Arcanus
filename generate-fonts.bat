@echo off
setlocal

clang -x c++ -std=c++17 -O2 -Wall -Wextra -Werror -o tools\fontgen.exe tools\fontgen.cpp
if errorlevel 1 exit /b 1

pushd resources
..\tools\fontgen.exe
set ERR=%ERRORLEVEL%
popd

exit /b %ERR%
