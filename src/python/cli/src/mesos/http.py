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

# Helper for doing an HTTP GET given a PID, a path, and a query dict.
# For example:
#
#     get('foo@1.2.3.4:123',
#         '/endpoint',
#         {'first': 'ben',
#          'last': 'hindman'})
#
# Would yield: 1.2.3.4:123/endpoint?first='ben'&last='hindman'
#
# Note that you can also pass an IP:port (or hostname:port) for 'pid'
# (i.e., you can omit the ID component of the PID, e.g., 'foo@').
def get(pid, path, query=None):
    import urllib2

    from contextlib import closing

    url = 'http://' + pid[(pid.find('@') + 1):] + path

    if query is not None and len(query) > 0:
        url += '?' + '&'.join(
            ['%s=%s' % (urllib2.quote(str(key)), urllib2.quote(str(value)))
             for (key, value) in query.items()])

    with closing(urllib2.urlopen(url)) as file:
        return file.read()
