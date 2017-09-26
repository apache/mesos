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

import cli.http as http

from cli.exceptions import CLIException
from cli.plugins import PluginBase
from cli.util import Table

PLUGIN_NAME = "task"
PLUGIN_CLASS = "Task"

VERSION = "Mesos CLI Task Plugin"

SHORT_HELP = "Interacts with the tasks running in a Mesos cluster"


class Task(PluginBase):
    """
    The task plugin.
    """

    COMMANDS = {
        "list": {
            "arguments": [],
            "flags": {},
            "short_help": "List all active tasks in a Mesos cluster",
            "long_help": "List all active tasks in a Mesos cluster"
        }
    }

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
            tasks = http.get_json(master, "tasks")["tasks"]
        except Exception as exception:
            raise CLIException("Could not open '/tasks'"
                               " endpoint at '{addr}': {error}"
                               .format(addr=master, error=exception))

        if len(tasks) == 0:
            print "There are no tasks running in the cluster."
            return

        try:
            table = Table(["Task ID", "Framework ID", "Executor ID"])
            for task in tasks:
                table.add_row([task["id"],
                               task["framework_id"],
                               task["executor_id"]])
        except Exception as exception:
            raise CLIException("Unable to build table of tasks: {error}"
                               .format(error=exception))

        print str(table)
