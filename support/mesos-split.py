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
Errors if a list of files spans across
the projects which make up mesos.
"""

from collections import defaultdict
import sys

if len(sys.argv) < 2:
    print("Usage: ./mesos-split.py <filename>...")

BASE_PROJECT = "mesos"

SUBPROJECTS = {
    "libprocess": "3rdparty/libprocess",
    "stout": "3rdparty/stout"
}

ERROR = """ERROR: Commit spanning multiple projects.

Please use separate commits for mesos, libprocess and stout.

Paths grouped by project:"""


def find_project(filename):
    """Find a project using its filename."""

    # Find longest prefix match.
    found_path_len = 0
    found_project = BASE_PROJECT
    for project, path in SUBPROJECTS.items():
        if filename.startswith(path) and len(path) > found_path_len:
            found_path_len = len(path)
            found_project = project

    return found_project


def main():
    """
    Expects a list of filenames on the command line.
    """
    touched_projects = defaultdict(list)
    for filename in sys.argv[1:]:
        touched_projects[find_project(filename)].append(filename)

    if len(touched_projects) > 1:
        print(ERROR)
        for project in touched_projects.keys():
            print("%s:" % project)
            for filename in touched_projects[project]:
                print("  %s" % filename)
        sys.exit(1)

    sys.exit(0)

if __name__ == '__main__':
    main()
