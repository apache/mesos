REM Licensed to the Apache Software Foundation (ASF) under one
REM or more contributor license agreements.  See the NOTICE file
REM distributed with this work for additional information
REM regarding copyright ownership.  The ASF licenses this file
REM to you under the Apache License, Version 2.0 (the
REM "License"); you may not use this file except in compliance
REM with the License.  You may obtain a copy of the License at
REM
REM     http://www.apache.org/licenses/LICENSE-2.0
REM
REM Unless required by applicable law or agreed to in writing, software
REM distributed under the License is distributed on an "AS IS" BASIS,
REM WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
REM See the License for the specific language governing permissions and
REM limitations under the License.


REM Usage: `VsBuildCommand.bat solution_file configuration project_root projects`
REM The arguments follow:
REM   * solution_file: The `.sln` file we want to build.
REM   * configuration: Type of build, usually `Debug` or `Release`.
REM   * project_root: Path to the `.vcxproj` files for the `.sln` in the first
REM     argument.
REM   * projects: List of projects to compile. For example, the ZK list is just
REM     "zookeeper", even though there is also a `Cli` project; the reason is
REM     that we don't need the `Cli` project to build Mesos.
REM
REM NOTE: This script is fairly limited in 2 ways: it assumes that (1) all the
REM VS project files for a solution are in `project_root`, and (2) all the
REM project files are `.vcxproj` files (rather than, say, `.vcproj` files). This
REM limitation can be overcome with more complicated command line parsing
REM machinery, since our use case is fairly limited, we did not think the
REM complexity was worth it.

@echo off

REM Parse command line params.
for /f "tokens=1-3*" %%A in ("%*") do (
  set SOLUTION_FILE=%%A
  set CONFIGURATION=%%B
  set PROJECTS_ROOT=%%C
  set PROJECTS=%%D
)

set PLATFORM=x64
setlocal EnableDelayedExpansion
setlocal EnableExtensions

REM Get Visual Studio 2015 install directory.
set VS2015_KEY=HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\14.0
set VS2015_INSTALLDIR_VALUE=InstallDir

for /F "skip=2 tokens=1,2*" %%A ^
in ('REG QUERY %VS2015_KEY% /v %VS2015_INSTALLDIR_VALUE% 2^>nul') do (
  set VS2015_DIR=%%C..\..
)

REM Check if Visual Studio 2015 is installed.
if defined VS2015_DIR (
  set SOLUTION_VER=12.00
  set PROJECT_VER=14.00
  REM Prepare Visual Studio 2015 command line environment.
  call "%VS2015_DIR%\VC\vcvarsall.bat" x86
) else (
  echo "No compiler : Microsoft Visual Studio (2015) is not installed."
  exit /b 1
)

REM For each project in the list of projects...
for %%P in (%PROJECTS%) do (
     devenv /upgrade %PROJECTS_ROOT%\%%P.vcxproj
)

FINDSTR ^
    /C:"Microsoft Visual Studio Solution File, Format Version %SOLUTION_VER%" ^
    %SOLUTION_FILE:/=\%
if %errorlevel% neq 0 (
  REM Upgrade solution file if its version does not match current %SOLUTION_VER%.
  echo "Upgrading Visual Studio Solution File: %SOLUTION_FILE% ."
  devenv /upgrade %SOLUTION_FILE%
)

if not "%PROJECTS%" == "" (
  set PROJECTS_TARGET=/t:%PROJECTS: =;%
)

msbuild ^
    %SOLUTION_FILE% %PROJECTS_TARGET% ^
    /p:Configuration=%CONFIGURATION%;Platform=%PLATFORM% ^
    /p:PlatformTarget=%PLATFORM% ^
    /p:RuntimeLibrary=MT_StaticDebug

:end
exit /b