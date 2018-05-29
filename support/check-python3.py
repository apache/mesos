#!/usr/bin/env python
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

"""
Checks if Python 3.6 is available on the machine.
"""
# pylint: disable=superfluous-parens

from __future__ import print_function

import os
import subprocess
import sys

def _print(message):
  print(message, file=sys.stderr)

def print_error():
    """Prints a warning requesting to install Python 3.6."""
    _print("The support scripts will be upgraded to Python 3 by July 1st.")
    _print("Make sure to install Python 3.6 on your machine before.")

def print_warning():
    """Prints a warning requesting to use the Python 3 scripts."""
    _print("Congratulations! You have Python 3 installed correctly.")
    _print("Please start using the scripts in `support/python3`.")
    # NOTE: This is only either unset, or set to 3.
    if "MESOS_SUPPORT_PYTHON" not in os.environ:
        _print("Please also set the environment variable `MESOS_SUPPORT_PYTHON` to `3`")
        _print("so that the Git hooks use the Python 3 scripts.")

if sys.version_info[0] < 3:
    # On Windows, system-wide installations of Python 3.6 gives a tools called
    # py and that we can use to know if Python 3 is installed.
    if os.name == "nt":
        PY = subprocess.call(["WHERE", "py"], stdout=open(os.devnull, "wb"))
    else:
        # We are not using Python 3 as python, let's check if python3 exists.
        PY = subprocess.call(["which", "python3"],
                             stdout=open(os.devnull, "wb"))
    if PY != 0:
        print_error()
    else:
        # It does exist, let's check its version.
        if os.name == "nt":
            VERSION = subprocess.check_output("py -3 --version", shell=True)
        else:
            VERSION = subprocess.check_output("python3 --version", shell=True)
        # x goes from 0 to 5 so that we can check for Python < 3.6.
        for x in range(0, 6):
            if "3.%d." % (x) in VERSION:
                print_error()
                sys.exit()
        # This script only gets invoked by the Python 2 scripts, so we
        # can assume we need to warn the user to start using the
        # Python 3 scripts.
        print_warning()
elif sys.version_info[1] < 6:
    # python is by default Python 3 but it's < 3.6.
    print_error()
