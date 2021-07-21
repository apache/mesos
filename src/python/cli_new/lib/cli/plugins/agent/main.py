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
The agent plugin.
"""

from cli.exceptions import CLIException
from cli.mesos import get_agents
from cli.plugins import PluginBase
from cli.util import Table


PLUGIN_NAME = "agent"
PLUGIN_CLASS = "Agent"

VERSION = "Mesos CLI Agent Plugin"

SHORT_HELP = "Interacts with the Mesos agents"


class Agent(PluginBase):
    """
    The agent plugin.
    """

    COMMANDS = {
        "list": {
            "arguments": [],
            "flags": {},
            "short_help": "List the Mesos agents.",
            "long_help": "List information about the Mesos agents."
        }
    }

    def list(self, argv):
        """
        List the agents in a cluster by checking the /slaves endpoint.
        """
        # pylint: disable=unused-argument
        try:
            master = self.config.master()
            config = self.config
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        try:
            agents = get_agents(master, config)
        except Exception as exception:
            raise CLIException("Unable to get agents from leading"
                               " master '{master}': {error}"
                               .format(master=master, error=exception))

        if not agents:
            print("The cluster does not have any agents.")
            return

        try:
            table = Table(["Agent ID", "Hostname", "Active"])
            for agent in agents:
                table.add_row([agent["id"],
                               agent["hostname"],
                               str(agent["active"])])
        except Exception as exception:
            raise CLIException("Unable to build table of agents: {error}"
                               .format(error=exception))

        print(str(table))
