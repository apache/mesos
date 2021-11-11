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
The framework plugin.
"""

import json

from cli.exceptions import CLIException
from cli.mesos import get_frameworks
from cli.plugins import PluginBase
from cli.util import Table


PLUGIN_NAME = "framework"
PLUGIN_CLASS = "Framework"

VERSION = "v0.1.0"

SHORT_HELP = "Interacts with the Mesos Frameworks"


class Framework(PluginBase):
    """
    The framework plugin.
    """

    COMMANDS = {
        "list": {
            "arguments": [],
            "flags": {
                "-a --all": "include inactive frameworks"
            },
            "short_help": "List the Mesos frameworks.",
            "long_help": "List information about the Mesos frameworks."
        },
        "inspect": {
            "arguments": ['<framework_id>'],
            "flags": {},
            "short_help": "Return low-level information on the framework.",
            "long_help": "Return low-level information on the framework."
        }
    }

    def list(self, argv):
        """
        Show a list of running frameworks
        """

        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        data = get_frameworks(master, self.config)
        table = Table(["ID", "Active", "Hostname", "Name"])
        for framework in data:
            if (not argv["--all"] and not framework["active"]):
                continue

            active = "False"
            if framework["active"]:
                active = "True"

            table.add_row([framework["id"],
                           active,
                           framework["hostname"],
                           framework["name"]])

        print(str(table))

    def inspect(self, argv):
        """
        Show the low-level information of the framework.
        """

        try:
            master = self.config.master()
        except Exception as exception:
            raise CLIException("Unable to get leading master address: {error}"
                               .format(error=exception))

        data = get_frameworks(master, self.config)
        for framework in data:
            if framework["id"] != argv["<framework_id>"]:
                continue

            # remove not helpfull information
            framework.pop('tasks', None)
            framework.pop('unreachable_tasks', None)
            framework.pop('completed_tasks', None)

            print(json.dumps(framework, indent=4))


