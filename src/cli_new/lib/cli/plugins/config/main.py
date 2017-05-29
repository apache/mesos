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

import os
import sys

import cli

from cli.exceptions import CLIException
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
        },
        "show": {
            "arguments": [],
            "flags": {},
            "short_help": "Show the contents of the configuration file.",
            "long_help": "Show the contents of the configuration file."
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
        plugins = cli.util.import_modules(
            cli.util.join_plugin_paths(self.settings, self.config),
            "plugins")

        for plugin in plugins:
            plugins_table.add_row([
                cli.util.get_module(plugins, plugin).PLUGIN_NAME,
                cli.util.get_module(plugins, plugin).SHORT_HELP
            ])
        sys.stdout.write("{}\n".format(plugins_table))

    def show(self, argv):
        """
        Show the contents of the configuration file.
        """
        # pylint: disable=unused-argument
        config_file = self.config.path
        if not os.path.isfile(config_file):
            raise CLIException("Unable to show the config file,"
                               " '{path}' does not exist"
                               .format(path=config_file))

        with open(config_file, 'r') as stream:
            try:
                sys.stdout.write("{stream}\n"
                                 .format(stream=stream.read().rstrip()))
            except Exception as exception:
                raise CLIException("Unable to read config file '{path}':"
                                   " {error}"
                                   .format(path=config_file, error=exception))
