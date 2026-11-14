@echo off
rem this file will be overwritten by the project setup script
setlocal
set "_SIMICS_PROJECT=C:\Users\axelr\simics-projects\my-intel-simics-project-4"
set PYTHONUTF8=1
set "SIMICS_PYTHON=%~dp0.\..\..\AppData\Local\Python\pythoncore-3.14-64\python.exe"
"C:\Users\axelr\simics-projects\my-intel-simics-project-4\.venv\Scripts\python.exe" %*
