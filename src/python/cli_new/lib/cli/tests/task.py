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
Task plugin tests.
"""

import cli.http as http

from cli import config

from cli.plugins.task.main import Task as TaskPlugin

from cli.tests import capture_output
from cli.tests import CLITestCase
from cli.tests import Agent
from cli.tests import Master
from cli.tests import Task

from cli.util import Table


class TestTaskPlugin(CLITestCase):
    """
    Test class for the task plugin.
    """
    def test_list(self):
        """
        Basic test for the task `list()` sub-command.
        """
        # Launch a master, agent, and task.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        task = Task({"command": "sleep 1000"})
        task.launch()

        # Open the master's `/tasks` endpoint and read the
        # task information ourselves.
        tasks = http.get_json(master.addr, 'tasks')["tasks"]

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 1)

        # Invoke the task plugin `list()` command
        # and parse its output as a table.
        test_config = config.Config(None)
        plugin = TaskPlugin(None, test_config)
        output = capture_output(plugin.list, {})
        table = Table.parse(output)

        # Verify there are two rows in the table
        # and that they are formatted as expected,
        # with the proper task info in them.
        self.assertEqual(table.dimensions()[0], 2)
        self.assertEqual(table.dimensions()[1], 3)
        self.assertEqual("Task ID", table[0][0])
        self.assertEqual("Framework ID", table[0][1])
        self.assertEqual("Executor ID", table[0][2])
        self.assertEqual(tasks[0]["id"], table[1][0])
        self.assertEqual(tasks[0]["framework_id"], table[1][1])
        self.assertEqual(tasks[0]["executor_id"], table[1][2])

        # Kill the task, agent, and master.
        task.kill()
        agent.kill()
        master.kill()
