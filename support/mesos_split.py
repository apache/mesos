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

# Errors if a list of files spans across the projects which make up
# mesos.

if __name__ != "__main__":
  raise Exception("This is not a libraray")

from collections import defaultdict
import sys

if len(sys.argv) < 2:
  print "Usage: ./mesos-split.py <filename>..."

base_project = "mesos"
subprojects = {
  "libprocess": "3rdparty/libprocess",
  "stout": "3rdparty/libprocess/3rdparty/stout"
}
ERROR = """ERROR: Commit spanning multiple projects.

Please use separate commits for mesos, libprocess and stout.

Paths grouped by project:"""


def find_project(filename):
  # Find longest prefix match.
  found_path_len = 0
  found_project = base_project
  for project, path in subprojects.iteritems():
    if filename.startswith(path) and len(path) > found_path_len:
      found_path_len = len(path)
      found_project = project

  return found_project


touched_projects = defaultdict(list)
for filename in sys.argv[1:]:
  touched_projects[find_project(filename)].append(filename)

if len(touched_projects) > 1:
  print ERROR
  for project in touched_projects.iterkeys():
    print "%s:" % project
    for filename in touched_projects[project]:
      print "  %s" % filename
  sys.exit(1)

sys.exit(0)
