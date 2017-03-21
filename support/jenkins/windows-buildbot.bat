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

REM Run a script provided by Visual Studio which sets up a
REM couple environment variables needed by the build.
CALL "F:/Microsoft/Visual Studio CE 2015/VC/vcvarsall.bat"

@echo on

REM Recreate the `/tmp` directory needed by tests.
REM We do this to not leave behind junk in case the tests (inevitably) fail.
REM NOTE: We don't check if the delete actually worked.
RMDIR /q /s %CD:~0,3%tmp
MKDIR %CD:~0,3%tmp

REM We need to specify the path to GNU Patch,
REM since it is installed in a non-default location.
SET OTHER_CMAKE_OPTIONS=-DPATCHEXE_PATH="F:/Program Files (x86)/GnuWin32/bin"

REM These stout tests require Administrator privileges.
REM Since we don't run "ROOT" tests on the ASF CI, these need to be disabled.
SET GTEST_FILTER=-FsTest.Symlink:FsTest.Rm:RmdirTest.RemoveDirectoryHangingSymlink:RmdirTest.RemoveDirectoryWithSymbolicLinkTargetDirectory:RmdirTest.RemoveDirectoryWithSymbolicLinkTargetFile

REM Call the Windows build script.
"support/windows-build.bat"
