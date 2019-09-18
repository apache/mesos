#!/usr/bin/env python3

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
"""This is a dummy script to ease the transition from mesos-style to pre-commit
hooks. This script can be removed after we have given contributors enough time
to adjust their checkouts."""

import sys

error = """\
'mesos-style.py' was removed in favor of hooks managed by pre-commit.

Linting requires an installation of pre-commit, see
https://pre-commit.com/#install, e.g.,

    $ pip3 install pre-commit

After installing pre-commit, remove existing hooks in '.git/hooks'
and reinstall hooks with './support/setup-dev.sh'.

    $ rm -rfv .git/hooks/*
    $ ./support/setup-dev.sh
"""

print(error, file=sys.stderr)
sys.exit(1)
