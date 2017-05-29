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
A collection of http related functions used by the CLI and its Plugins.
"""

import json
import urllib2
import time

from cli.exceptions import CLIException


def read_endpoint(addr, endpoint):
    """
    Read the specified endpoint and return the results.
    """
    try:
        url = "http://{addr}/{endpoint}".format(addr=addr, endpoint=endpoint)

        http_response = urllib2.urlopen(url).read().decode("utf-8")
    except Exception as exception:
        raise CLIException("{error}".format(error=str(exception)))

    return http_response


def get_json(addr, endpoint, condition=None, timeout=5):
    """
    Return the contents of the 'endpoint' at 'addr' as JSON data
    subject to the condition specified in 'condition'. If we are
    unable to read the data or unable to meet the condition within
    'timeout' seconds we throw an error.
    """
    start_time = time.time()

    while True:
        data = None

        try:
            data = read_endpoint(addr, endpoint)
        except Exception as exception:
            pass

        if data:
            try:
                data = json.loads(data)
            except Exception as exception:
                raise CLIException("Could not load JSON from '{data}': {error}"
                                   .format(data=data, error=str(exception)))

            if not condition:
                return data

            if condition(data):
                return data

        if time.time() - start_time > timeout:
            raise CLIException("Failed to get data within {seconds} seconds:"
                               " {error}"
                               .format(seconds=str(timeout), error=exception))

        time.sleep(0.1)
