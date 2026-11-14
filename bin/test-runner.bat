
@echo off
rem this file will be overwritten by the project setup script
setlocal
set SIMICS_BASE_PACKAGE=C:\Users\axelr\AppData\Local\Programs\Simics\simics-7.84.0
set SIMICS_PYTHON_PACKAGE=C:\Users\axelr\AppData\Local\Python\pythoncore-3.14-64
set "SIMICS_PYTHON=%~dp0..\python.bat"
if exist "C:\Users\axelr\simics-projects\my-intel-simics-project-4\.package-list" set SIMICS_EPL=--package-list "C:\Users\axelr\simics-projects\my-intel-simics-project-4\.package-list"
if not exist "C:\Users\axelr\simics-projects\my-intel-simics-project-4\.package-list" set SIMICS_EPL=
"C:\Users\axelr\AppData\Local\Programs\Simics\simics-7.84.0\bin\test-runner.bat" %SIMICS_EPL% --project "C:\Users\axelr\simics-projects\my-intel-simics-project-4" %*
