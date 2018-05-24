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

import os
import subprocess
import sys


def print_error():
    """Prints a warning requesting to install Python 3.6."""
    print("The support scripts will be upgraded to Python 3 by July 1st.")
    print("Make sure to install Python 3.6 on your machine before.")

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
elif sys.version_info[1] < 6:
    # python is by default Python 3 but it's < 3.6.
    print_error()
