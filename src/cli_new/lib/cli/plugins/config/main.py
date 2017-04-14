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
The config plugin.
"""

import sys

import settings
import cli

from cli.plugins import PluginBase
from cli.util import Table


PLUGIN_NAME = "config"
PLUGIN_CLASS = "Config"

VERSION = "Mesos CLI Config Plugin"

SHORT_HELP = "Interacts with the Mesos CLI configuration file"


class Config(PluginBase):
    """
    The config plugin.
    """

    COMMANDS = {
        "plugins": {
            "arguments": [],
            "flags": {},
            "short_help": "Print the plugins that can be used.",
            "long_help": "Print the plugins that can be used."
        }
    }

    def plugins(self, argv):
        """
        Parse and load the builtin plugins and the ones in the configuration
        file. If this method is called using 'mesos config plugins', it
        displays the plugins that can be used.
        """
        # pylint: disable=unused-argument
        plugins_table = Table(["NAME", "DESCRIPTION"])

        # Load the plugins
        loaded_plugins = cli.util.import_modules(settings.PLUGINS, "plugins")
        for plugin in loaded_plugins:
            plugins_table.add_row([
                cli.util.get_module(loaded_plugins, plugin).PLUGIN_NAME,
                cli.util.get_module(loaded_plugins, plugin).SHORT_HELP
            ])
        sys.stdout.write("{}\n".format(plugins_table))
