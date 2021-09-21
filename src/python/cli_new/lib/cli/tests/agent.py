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
Agent plugin tests.
"""

from cli import config
from cli import http

from cli.plugins.agent.main import Agent as AgentPlugin

from cli.tests import capture_output
from cli.tests import CLITestCase
from cli.tests import Agent
from cli.tests import Master

from cli.util import Table


class TestAgentPlugin(CLITestCase):
    """
    Test class for the agent plugin.
    """
    def test_list(self):
        """
        Basic test for the agent `list()` sub-command.
        """
        # Launch a master and agent.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        # Open the master's `/slaves` endpoint and read the
        # agents' information ourselves.
        agents = http.get_json(master.addr, 'slaves',
                               config.Config(None))["slaves"]

        self.assertEqual(type(agents), list)
        self.assertEqual(len(agents), 1)

        # Invoke the agent plugin `list()` command
        # and parse its output as a table.
        test_config = config.Config(None)
        plugin = AgentPlugin(None, test_config)
        output = capture_output(plugin.list, {})
        table = Table.parse(output)

        # Verify there are two rows in the table
        # and that they are formatted as expected,
        # with the proper agent info in them.
        self.assertEqual(table.dimensions()[0], 2)
        self.assertEqual(table.dimensions()[1], 3)
        self.assertEqual("Agent ID", table[0][0])
        self.assertEqual("Hostname", table[0][1])
        self.assertEqual("Active", table[0][2])
        self.assertEqual(agents[0]["id"], table[1][0])
        self.assertEqual(agents[0]["hostname"], table[1][1])
        self.assertEqual(str(agents[0]["active"]), table[1][2])

        # Kill the agent and master.
        agent.kill()
        master.kill()
