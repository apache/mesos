:: Licensed to the Apache Software Foundation (ASF) under one
:: or more contributor license agreements.  See the NOTICE file
:: distributed with this work for additional information
:: regarding copyright ownership.  The ASF licenses this file
:: to you under the Apache License, Version 2.0 (the
:: "License"); you may not use this file except in compliance
:: with the License.  You may obtain a copy of the License at
::
::     http://www.apache.org/licenses/LICENSE-2.0
::
:: Unless required by applicable law or agreed to in writing, software
:: distributed under the License is distributed on an "AS IS" BASIS,
:: WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
:: See the License for the specific language governing permissions and
:: limitations under the License.

@echo off

:: Make sure that we are in the right directory. We do this by checking that
:: the `support` folder exists in the current directory and is not a symlink.
:: This code is awkwardly split across two conditionals because batch scripts
:: do not support logical operators like `&&`.
::
:: NOTE: The Linux equivalent (`bootstrap`, a script written in Bash) instead
:: checks that `configure.ac` exists and is not a symlink; since we expect to
:: deprecate the autotools build system, we choose not to verify the current
:: directory by checking that `configure.ac` script exists, since we expect it
:: to go away soon. Instead, we depend on finding the `support/` folder, which
:: we expect to be permanent (i.e., we expect to copy files from it anyway).
if not exist support (
  goto not_in_root
)

fsutil reparsepoint query "support" | find "Symbolic Link" >nul && (
  goto not_in_root
)

:: Install mesos default hooks and gitignore template.
if not exist .git\hooks\pre-commit (
  mklink .git\hooks\pre-commit ..\..\support\hooks\pre-commit
)

if not exist .git\hooks\post-rewrite (
  mklink .git\hooks\post-rewrite ..\..\support\hooks\post-rewrite
)

if not exist .gitignore (
  mklink .gitignore .gitignore-template
)

if not exist .reviewboardrc (
  mklink .reviewboardrc support\reviewboardrc
)

if not exist .clang-format (
  mklink .clang-format support\clang-format
)

goto:eof


:: If we are not in the root directory, print error and exit.
:not_in_root
echo. 1>&2
echo You must run bootstrap from the root of the distribution. 1>&2
echo. 1>&2

exit /b 1
