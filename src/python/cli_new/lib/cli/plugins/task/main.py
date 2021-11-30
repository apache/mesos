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
The task plugin.
"""

import json

from cli.exceptions import CLIException
from cli.mesos import get_tasks
from cli.plugins import PluginBase
from cli.util import Table

from cli.mesos import TaskIO

PLUGIN_NAME = "task"
PLUGIN_CLASS = "Task"

VERSION = "Mesos CLI Task Plugin"

SHORT_HELP = "Interacts with the tasks running in a Mesos cluster"


class Task(PluginBase):
    """
    The task plugin.
    """

    COMMANDS = {
        "attach": {
            "arguments": ['<task-id>'],
            "flags": {
                "--no-stdin": "do not attach a stdin [default: False]"
            },
            "short_help": "Attach the CLI to the stdio of a running task",
            "long_help": """
                Attach the CLI to the stdio of a running task
                To detach type the sequence CTRL-p CTRL-q."""
        },
        "exec": {
            "arguments": ['<task-id>', '<command>', '[<args>...]'],
            "flags": {
                "-i --interactive" : "interactive [default: False]",
                "-t --tty": "tty [default: False]"
            },
            "short_help": "Execute commands in a task's container",
            "long_help": "Execute commands in a task's container"
        },
        "list": {
            "arguments": [],
            "flags": {
                "-a --all": "list all tasks, not only running [default: False]"
            },
            "short_help": "List all running tasks in a Mesos cluster",
            "long_help": "List all running tasks in a Mesos cluster"
        },
        "inspect": {
            "arguments": ['<task_id>'],
            "flags": {},
            "short_help": "Return low-level information on the task",
            "long_help": "Return low-level information on the task"
        }        
    }

    def attach(self, argv):
        """
        Attach the stdin/stdout/stderr of the CLI to the
        STDIN/STDOUT/STDERR of a running task.
        """
        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        task_io = TaskIO(master, self.config, argv["<task-id>"])
        return task_io.attach(argv["--no-stdin"])


    def exec(self, argv):
        """
        Launch a process inside a task's container.
        """
        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        task_io = TaskIO(master, self.config, argv["<task-id>"])
        return task_io.exec(argv["<command>"],
                            argv["<args>"],
                            argv["--interactive"],
                            argv["--tty"])

    def list(self, argv):
        """
        List the tasks running in a cluster by checking the /tasks endpoint.
        """
        # pylint: disable=unused-argument
        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        try:
            tasks = get_tasks(master, self.config)
        except Exception as exception:
            raise CLIException("Unable to get tasks from leading"
                               " master '{master}': {error}"
                               .format(master=master, error=exception))

        if not tasks:
            print("There are no tasks running in the cluster.")
            return

        try:
            table = Table(["ID", "State", "Framework ID", "Executor ID"])
            for task in tasks:
                task_state = "UNKNOWN"
                if task["statuses"]:
                    task_state = task["statuses"][-1]["state"]

                if not argv["--all"] and task_state != "TASK_RUNNING":
                    continue

                table.add_row([task["id"],
                               task_state,
                               task["framework_id"],
                               task["executor_id"]])
        except Exception as exception:
            raise CLIException("Unable to build table of tasks: {error}"
                               .format(error=exception))

        print(str(table))

    def inspect(self, argv):
        """
        Show the low-level information on the task.
        """
        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        data = get_tasks(master, self.config)
        for task in data:
            if task["id"] != argv["<task_id>"]:
                continue

            print(json.dumps(task, indent=4))

