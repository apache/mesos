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

@echo on

REM NOTE: Before you run this script, you must have the Visual Studio
REM environment variables set up. Visual Studio provides a script to do this,
REM depending on the version of Visual Studio installed:
REM /path/to/Visual Studio 14/VC/vcvarsall.bat
REM /path/to/Microsoft Visual Studio/2017/Community/VC/Auxiliary/Build/vcvars64.bat

REM NOTE: Batch doesn't have any way of exiting upon failing a command.
REM The best we can do is add this line after every command that matters:
REM if %errorlevel% neq 0 exit /b %errorlevel%

REM NOTE: In order to run the tests, your Windows volume must have a
REM folder named `tmp` at the top level. You can create it like this:
REM MKDIR %CD:~0,3%tmp

REM Make sure that we are in the right directory. We do this by checking that
REM the `support` folder exists in the current directory and is not a symlink.
REM This code is awkwardly split across two conditionals because batch scripts
REM do not support logical operators like `&&`.
if not exist support (
  goto not_in_root
)

fsutil reparsepoint query "support" | find "Symbolic Link" >nul && (
  goto not_in_root
)

REM Create a build directory.
MKDIR build
CD build
if %errorlevel% neq 0 exit /b %errorlevel%

REM Generate the Visual Studio solution.
REM You can pass in other flags by setting `OTHER_CMAKE_OPTIONS` before
REM calling the script. For example, the ASF CI will add `-DPATCHEXE_PATH=...`
REM because the path to GNU Patch is not the default.
if not defined CMAKE_GENERATOR (set CMAKE_GENERATOR=Visual Studio 15 2017 Win64)
cmake .. -G "%CMAKE_GENERATOR%" -T "host=x64" -DENABLE_LIBEVENT=1 %OTHER_CMAKE_OPTIONS%
if %errorlevel% neq 0 exit /b %errorlevel%

REM Build and run the stout tests.
cmake --build . --target stout-tests --config Debug
if %errorlevel% neq 0 exit /b %errorlevel%

"3rdparty/stout/tests/Debug/stout-tests.exe"
if %errorlevel% neq 0 exit /b %errorlevel%

REM Build and run the libprocess tests.
cmake --build . --target libprocess-tests --config Debug
if %errorlevel% neq 0 exit /b %errorlevel%

"3rdparty/libprocess/src/tests/Debug/libprocess-tests.exe"
if %errorlevel% neq 0 exit /b %errorlevel%

REM Build and run the mesos tests.
cmake --build . --target mesos-tests --config Debug
if %errorlevel% neq 0 exit /b %errorlevel%

REM Due to how Mesos uses and creates symlinks, the next test suite
REM will only pass when run as an Administrator. The following command
REM is a read-only command that only passes for Administrators.
REM See: https://technet.microsoft.com/en-us/library/bb490711.aspx
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator permissions not detected.  Skipping Mesos tests...
) else (
    REM Run mesos tests.
    "src/mesos-tests.exe" --verbose
    if %errorlevel% neq 0 exit /b %errorlevel%
)

goto :eof

REM If we are not in the root directory, print error and exit.
:not_in_root
echo. 1>&2
echo You must run windows-build from the root of the distribution. 1>&2
echo. 1>&2

exit /b 1
