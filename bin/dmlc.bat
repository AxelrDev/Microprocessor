@echo off
rem this file will be overwritten by the project setup script
setlocal
set SIMICS_BASE_PACKAGE=C:\Users\axelr\AppData\Local\Programs\Simics\simics-7.84.0
set SIMICS_PYTHON_PACKAGE=C:\Users\axelr\AppData\Local\Python\pythoncore-3.14-64
set "SIMICS_PYTHON=%~dp0..\python.bat"
"C:\Users\axelr\AppData\Local\Programs\Simics\simics-7.84.0\bin\dmlc.bat"  %*
