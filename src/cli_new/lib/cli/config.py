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
Config class to manage the configuration file.
"""

import os
import toml

import settings
from cli.exceptions import CLIException


class Config(object):
    """
    The Config class loads the configuration file on initialization and has
    one method for each element that can be specified in the config file.
    """

    def __init__(self):
        # Load the configuration file path for the CLI.
        if os.environ.get("MESOS_CLI_CONFIG"):
            self.path = os.environ["MESOS_CLI_CONFIG"]
        else:
            self.path = settings.DEFAULT_MESOS_CLI_CONFIG

        # Load the configuration file as a TOML file.
        try:
            self.data = toml.load(self.path)
        except Exception as exception:
            raise CLIException("Error loading config file as TOML: {error}"
                               .format(error=exception))

    def plugins(self):
        """
        Parse the plugins listed in the configuration file and return them.
        """
        # Allow extra plugins to be pulled in from the configuration file.
        if "plugins" in self.data:
            if not isinstance(self.data["plugins"], list):
                raise CLIException("Unable to parse config file '{path}':"
                                   " 'plugins' field must be a list"
                                   .format(path=self.path))

            for plugin in self.data["plugins"]:
                if not os.path.exists(plugin):
                    raise CLIException("Plugin path not found: {path}"
                                       .format(path=plugin))
            return self.data["plugins"]

        return []
