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
Functions to handle agents.
"""

from cli import http
from cli.exceptions import CLIException


def get_agent_address(agent_id, master):
    """
    Given a master and an agent id, return the agent address
    by checking the /slaves endpoint of the master.
    """
    try:
        agents = http.get_json(master, "slaves")["slaves"]
    except Exception as exception:
        raise CLIException("Could not open '/slaves'"
                           " endpoint at '{addr}': {error}"
                           .format(addr=master,
                                   error=exception))
    for agent in agents:
        if agent["id"] == agent_id:
            return agent["pid"].split("@")[1]
    raise CLIException("Unable to find agent '{id}'".format(id=agent_id))


def get_agents(master):
    """
    Get the agents in a Mesos cluster.
    """
    endpoint = "slaves"
    key = "slaves"

    try:
        data = http.get_json(master, endpoint)
    except Exception as exception:
        raise CLIException(
            "Could not open '/{endpoint}' on master address '{addr}': {error}"
            .format(endpoint=endpoint, addr=master, error=exception))

    if not key in data:
        raise CLIException(
            "Missing '{key}' key in data retrieved"
            " from master on '/{endpoint}'"
            .format(key=key, endpoint=endpoint))

    return data[key]


def get_tasks(master):
    """
    Get the tasks in a Mesos cluster.
    """
    endpoint = "tasks"
    key = "tasks"

    try:
        data = http.get_json(master, endpoint)
    except Exception as exception:
        raise CLIException(
            "Could not open '/{endpoint}' on master address '{addr}': {error}"
            .format(endpoint=endpoint, addr=master, error=exception))

    if not key in data:
        raise CLIException(
            "Missing '{key}' key in data retrieved"
            " from master on '/{endpoint}'"
            .format(key=key, endpoint=endpoint))

    return data[key]
