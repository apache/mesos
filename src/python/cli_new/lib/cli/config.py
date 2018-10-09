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

import cli

from cli.constants import DEFAULT_MASTER_IP
from cli.constants import DEFAULT_MASTER_PORT
from cli.exceptions import CLIException


class Config():
    """
    The Config class loads the configuration file on initialization and has
    one method for each element that can be specified in the config file.
    """

    def __init__(self, settings):
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

    def master(self):
        """
        Parse the master info in the configuration file and return
        its IP address and the port where Mesos is running.
        """
        master = "{ip}:{port}".format(ip=DEFAULT_MASTER_IP,
                                      port=DEFAULT_MASTER_PORT)

        if "master" in self.data:
            if not isinstance(self.data["master"], dict):
                raise CLIException("The 'master' field must be a dictionary")

            if ("address" not in self.data["master"] and
                    "zookeeper" not in self.data["master"]):
                raise CLIException("The 'master' field must either"
                                   " contain an 'address' field or"
                                   " a 'zookeeper' dictionary")
            if ("address" in self.data["master"] and
                    "zookeeper" in self.data["master"]):
                raise CLIException("The 'master' field should only contain "
                                   " an 'address' field or a 'zookeeper'"
                                   " dictionary but not both")

            if "address" in self.data["master"]:
                master_address = self.data["master"]["address"]
                try:
                    master = cli.util.sanitize_address(master_address)
                except Exception as exception:
                    raise CLIException("The 'master' address {address} is"
                                       " formatted incorrectly: {error}"
                                       .format(address=master_address,
                                               error=exception))

            if "zookeeper" in self.data["master"]:
                zk_field = self.data["master"]["zookeeper"]

                if ("addresses" not in zk_field or
                        not isinstance(zk_field["addresses"], list)):
                    raise CLIException("The 'zookeeper' field must contain"
                                       " an 'addresses' list")

                if ("path" not in zk_field or
                        not isinstance(zk_field["path"], str)):
                    raise CLIException("The 'zookeeper' field must contain"
                                       " a 'path' field")

                if not zk_field["path"].startswith("/"):
                    raise CLIException("The 'zookeeper' field 'path'"
                                       " must start with a '/'")
                if len(zk_field["path"]) == 1:
                    raise CLIException("The 'zookeeper' field 'path' should"
                                       " be nested ('/' is not supported)")

                for address in zk_field["addresses"]:
                    try:
                        cli.util.sanitize_address(address)
                    except Exception as exception:
                        raise CLIException("The 'zookeeper' address {address}"
                                           " is formatted incorrectly: {error}"
                                           .format(address=address,
                                                   error=exception))
                try:
                    master = cli.util.zookeeper_resolve_leader(
                        zk_field["addresses"], zk_field["path"])
                except Exception as exception:
                    raise CLIException("Could not resolve the"
                                       " leading master: {error}"
                                       .format(error=exception))

        return master

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
