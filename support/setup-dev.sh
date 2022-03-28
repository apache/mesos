#!/bin/sh

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Make sure that we are in the right directory.
if test ! -f configure.ac; then
    cat >&2 <<__EOF__

You must run 'support/setup-dev.sh' from the root of the distribution.

__EOF__
    exit 1
fi

if test ! -e .gitignore; then
  # Git version 2.32.0 does not support gitignore as symlink (https://github.com/git/git/blob/master/Documentation/RelNotes/2.32.0.txt)
  cp support/gitignore .gitignore
fi

if test ! -e .reviewboardrc; then
  ln -s support/reviewboardrc .reviewboardrc
fi

if test ! -e .clang-format; then
  ln -s support/clang-format .clang-format
fi

if test ! -e .clang-tidy; then
  ln -s support/clang-tidy .clang-tidy
fi

if test ! -e CPPLINT.cfg; then
  ln -s support/CPPLINT.cfg CPPLINT.cfg
fi

if test ! -e .gitlint; then
  ln -s support/gitlint .gitlint
fi

if test ! -e .pre-commit-config.yaml; then
  ln -s support/pre-commit-config.yaml .pre-commit-config.yaml
fi

# Install Mesos default hooks and gitignore template.
if ! command pre-commit; then
   cat >&2 <<__EOF__

Please install pre-commit to excercise Mesos linters, https://pre-commit.com/#install.
__EOF__
    exit 1
fi

pre-commit install-hooks
pre-commit install --hook-type pre-commit
pre-commit install --hook-type commit-msg
