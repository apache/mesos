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

# Helper functions to get the absolute path of a directory as well as compute
# the relative path between two directories.
# Unfortunately these functions are not as readable as, say, python's
# os.path.relpath method, but they allow us to avoid a hard dependence on
# python or other tools during bootstrap.
abspath() {
    cd "`dirname "${1}"`"
    echo "${PWD}"/"`basename "${1}"`"
    cd "${OLDPWD}"
}
relpath() {
  local from to up
  from="`abspath "${1%/}"`" to="`abspath "${2%/}"/`"
  while test "${to}"  = "${to#"${from}"/}" \
          -a "${to}" != "${from}"; do
    from="${from%/*}" up="../${up}"
  done
  to="${up%/}${to#${from}}"
  echo "${to:-.}"
}

# Grab a reference to the repo's git directory. Usually this is simply .git in
# the repo's top level directory. However, when submodules are used, it may
# appear elsewhere. The most up-to-date way of finding this directory is to use
# `git rev-parse --git-common-dir`. This is necessary to support things like
# git worktree in addition to git submodules. However, as of January 2016,
# support for the '--git-common-dir' flag is fairly new, forcing us to fall
# back to the older '--git-dir' flag if '--git-common-dir' is not supported. We
# do this by checking the output of `git rev-parse --git-common-dir` and seeing
# if it gives us a valid directory back. If not, we set the git directory using
# the '--git-dir' flag instead.
_gitdir=`git rev-parse --git-common-dir`
if test ! -d "${_gitdir}"; then
  _gitdir=`git rev-parse --git-dir`
fi

# Grab a reference to the git hooks directory as well as the relative path from
# the git hooks directory to the current directory.
_hooksdir=${_gitdir}/hooks
_relpath=`relpath "${_hooksdir}" "${PWD}"`

# Install mesos default hooks and gitignore template.
if test ! -e "${_hooksdir}/pre-commit"; then
  ln -s "${_relpath}/support/hooks/pre-commit" "${_hooksdir}/pre-commit"
fi

if test ! -e "${_hooksdir}/post-rewrite"; then
  ln -s "${_relpath}/support/hooks/post-rewrite" "${_hooksdir}/post-rewrite"
fi

if test ! -e "${_hooksdir}/commit-msg"; then
  ln -s "${_relpath}/support/hooks/commit-msg" "${_hooksdir}/commit-msg"
fi


if test ! -e .gitignore; then
  ln -s support/gitignore .gitignore
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
