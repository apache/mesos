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
from urllib.parse import urlencode
import urllib3

import cli

from cli.exceptions import CLIException

# Disable all SSL warnings. These are not necessary, as the user has
# the option to disable SSL verification.
urllib3.disable_warnings()

def read_endpoint(addr, endpoint, config, query=None):
    """
    Read the specified endpoint and return the results.
    """

    try:
        addr = cli.util.sanitize_address(addr)
    except Exception as exception:
        raise CLIException("Unable to sanitize address '{addr}': {error}"
                           .format(addr=addr, error=str(exception)))
    try:
        url = "{addr}/{endpoint}".format(addr=addr, endpoint=endpoint)
        if query is not None:
            url += "?{query}".format(query=urlencode(query))
        if config.principal() is not None and config.secret() is not None:
            headers = urllib3.make_headers(
                basic_auth=config.principal() + ":" + config.secret()
            )
        else:
            headers = None
        http = urllib3.PoolManager()
        http_response = http.request(
            'GET',
            url,
            headers=headers,
            timeout=config.agent_timeout()
        )
        return http_response.data.decode('utf-8')

    except Exception as exception:
        raise CLIException("Unable to open url '{url}': {error}"
                           .format(url=url, error=str(exception)))


def get_json(addr, endpoint, config, query=None):
    """
    Return the contents of the 'endpoint' at 'addr' as JSON data.
    If we are unable to read the data we throw an error.
    """
    data = read_endpoint(addr, endpoint, config, query)

    try:
        data = json.loads(data)
    except Exception as exception:
        raise CLIException("Could not load JSON from '{data}': {error}"
                           .format(data=data, error=str(exception)))

    return data
