#!/usr/bin/env python3
#
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
A utility that expects JSON data at a particular URL and lets you
recursively extract keys from the JSON object as specified on the
command line (each argument on the command line after the first will
be used to recursively index into the JSON object). The name is a
play off of 'curl'.
"""

import json
import sys
import urllib.request
import urllib.error
import urllib.parse


def main():
    """Expects at least one argument on the command line."""
    if len(sys.argv) < 2:
        print("USAGE: {} URL [KEY...]".format(sys.argv[0]), file=sys.stderr)
        sys.exit(1)

    url = sys.argv[1]

    data = json.loads(urllib.request.urlopen(url).read())

    for arg in sys.argv[2:]:
        try:
            temp = data[arg]
            data = temp
        except KeyError:
            print("'" + arg + "' was not found", file=sys.stderr)
            sys.exit(1)

    print(data.encode("utf-8"))

if __name__ == '__main__':
    main()
