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

import os

from cli import config
from cli import http

from cli.exceptions import CLIException

from cli.plugins.task.main import Task as TaskPlugin

from cli.tests import capture_output
from cli.tests import exec_command
from cli.tests import wait_for_task
from cli.tests import CLITestCase
from cli.tests import Agent
from cli.tests import Master
from cli.tests import Task

from cli.tests.constants import TEST_DATA_DIRECTORY

from cli.util import Table


LOREM_IPSUM = os.path.join(TEST_DATA_DIRECTORY, "lorem-ipsum.txt")

class TestTaskPlugin(CLITestCase):
    """
    Test class for the task plugin.
    """
    def test_exec(self):
        """
        Basic test for the task `exec()` sub-command.
        """
        # Launch a master, agent, and task.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        with open(LOREM_IPSUM) as text:
            content = text.read()
        command = "printf '{data}' > a.txt && sleep 1000".format(data=content)
        task = Task({"command": command})
        task.launch()

        try:
            wait_for_task(master, task.name, "TASK_RUNNING")
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task.name, state="TASK_RUNNING", error=exception))

        try:
            tasks = http.get_json(master.addr, None, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException(
                "Could not get tasks from '/{endpoint}' on master: {error}"
                .format(endpoint="tasks", error=exception))

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 1)

        returncode, stdout, stderr = exec_command(
            ["mesos", "task", "exec", tasks[0]["id"], "cat", "a.txt"])

        self.assertEqual(returncode, 0)
        self.assertEqual(stdout, content)
        self.assertEqual(stderr, "")

        # Kill the task, agent, and master.
        task.kill()
        agent.kill()
        master.kill()

    def test_exec_exit_status(self):
        """
        Basic test for the task `exec()` sub-command exit status.
        """
        # Launch a master, agent, and task.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        task = Task({"command": "sleep 1000"})
        task.launch()

        try:
            wait_for_task(master, task.name, "TASK_RUNNING")
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task.name, state="TASK_RUNNING", error=exception))

        try:
            tasks = http.get_json(master.addr, None, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException(
                "Could not get tasks from '/{endpoint}' on master: {error}"
                .format(endpoint="tasks", error=exception))

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 1)

        returncode, _, _ = exec_command(
            ["mesos", "task", "exec", tasks[0]["id"], "true"])
        self.assertEqual(returncode, 0)

        returncode, _, _ = exec_command(
            ["mesos", "task", "exec", tasks[0]["id"], "bash", "-c", "exit 10"])
        self.assertEqual(returncode, 10)

        # Kill the task, agent, and master.
        task.kill()
        agent.kill()
        master.kill()


    def test_exec_interactive(self):
        """
        Test for the task `exec()` sub-command, using `--interactive`.
        """
        # Launch a master, agent, and task.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        task = Task({"command": "sleep 1000"})
        task.launch()

        try:
            wait_for_task(master, task.name, "TASK_RUNNING")
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task.name, state="TASK_RUNNING", error=exception))

        try:
            tasks = http.get_json(master.addr, None, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException(
                "Could not get tasks from '/{endpoint}' on master: {error}"
                .format(endpoint="tasks", error=exception))

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 1)

        with open(LOREM_IPSUM) as text:
            returncode, stdout, stderr = exec_command(
                ["mesos", "task", "exec", "-i", tasks[0]["id"], "cat"],
                stdin=text)

        self.assertEqual(returncode, 0)
        with open(LOREM_IPSUM) as text:
            self.assertEqual(stdout, text.read())
        self.assertEqual(stderr, "")

        # Kill the task, agent, and master.
        task.kill()
        agent.kill()
        master.kill()


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

        try:
            wait_for_task(master, task.name, "TASK_RUNNING")
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task.name, state="TASK_RUNNING", error=exception))

        try:
            tasks = http.get_json(master.addr, None, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException(
                "Could not get tasks from '/{endpoint}' on master: {error}"
                .format(endpoint="tasks", error=exception))

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 1)

        # Invoke the task plugin `list()` command
        # and parse its output as a table.
        test_config = config.Config(None)
        plugin = TaskPlugin(None, test_config)
        output = capture_output(plugin.list, {"--all": False})
        table = Table.parse(output)

        # Verify there are two rows in the table
        # and that they are formatted as expected,
        # with the proper task info in them.
        self.assertEqual(table.dimensions()[0], 2)
        self.assertEqual(table.dimensions()[1], 4)
        self.assertEqual("ID", table[0][0])
        self.assertEqual("State", table[0][1])
        self.assertEqual("Framework ID", table[0][2])
        self.assertEqual("Executor ID", table[0][3])
        self.assertEqual(tasks[0]["id"], table[1][0])
        self.assertEqual(tasks[0]["statuses"][-1]["state"], table[1][1])
        self.assertEqual(tasks[0]["framework_id"], table[1][2])
        self.assertEqual(tasks[0]["executor_id"], table[1][3])

        # Kill the task, agent, and master.
        task.kill()
        agent.kill()
        master.kill()

    def test_list_all(self):
        """
        Basic test for the task `list()` sub-command with flag `--all`.
        """
        # Launch a master, agent, and two tasks.
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        task1 = Task({"command": "true"})
        task1.launch()
        task1_state = "TASK_FINISHED"

        try:
            wait_for_task(master, task1.name, task1_state)
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task1.name, state=task1_state, error=exception))

        task2 = Task({"command": "sleep 1000"})
        task2.launch()
        task2_state = "TASK_RUNNING"

        try:
            wait_for_task(master, task2.name, task2_state)
        except Exception as exception:
            raise CLIException(
                "Error waiting for task '{name}' to"
                " reach state '{state}': {error}"
                .format(name=task2.name, state=task2_state, error=exception))

        try:
            tasks = http.get_json(master.addr, None, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException(
                "Could not get tasks from '/{endpoint}' on master: {error}"
                .format(endpoint="tasks", error=exception))

        self.assertEqual(type(tasks), list)
        self.assertEqual(len(tasks), 2)

        # Invoke the task plugin `list()` command
        # and parse its output as a table.
        test_config = config.Config(None)
        plugin = TaskPlugin(None, test_config)
        output = capture_output(plugin.list, {"--all": True})
        table = Table.parse(output)

        # Verify that there are two rows in the table, one for the running task
        # and one for the finished task. We do verify the information in the
        # table as this is already covered in the test `test_list`.
        self.assertEqual(table.dimensions()[0], 3)
        self.assertEqual(table.dimensions()[1], 4)

        # Kill the task1, task2, agent, and master.
        task1.kill()
        task2.kill()
        agent.kill()
        master.kill()
