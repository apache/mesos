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
A collection of helper functions used by the CLI and its Plugins.
"""

import imp
import importlib
import os
import re
import socket
import textwrap

import cli.http as http

from cli.exceptions import CLIException


def import_modules(package_paths, module_type):
    """
    Looks for python packages under `package_paths` and imports
    them as modules. Returns a dictionary of the basename of the
    `package_paths` to the imported modules.
    """
    modules = {}
    for package_path in package_paths:
        # We put the imported module into the namespace of
        # "mesos.<module_type>.<>" to keep it from cluttering up
        # the import namespace elsewhere.
        package_name = os.path.basename(package_path)
        package_dir = os.path.dirname(package_path)
        module_name = "cli." + module_type + "." + package_name
        try:
            module = importlib.import_module(module_name)
        except Exception:
            obj, filename, data = imp.find_module(package_name, [package_dir])
            module = imp.load_module(module_name, obj, filename, data)
        modules[package_name] = module

    return modules


def get_module(modules, import_path):
    """
    Given a modules dictionary returned by `import_modules()`,
    return a reference to the module at `import_path` relative
    to the base module. For example, get_module(modules, "example.stuff")
    will return a reference to the "stuff" module inside the
    imported "example" plugin.
    """
    import_path = import_path.split('.')
    try:
        module = modules[import_path[0]]
        if len(import_path) > 1:
            module = getattr(module, ".".join(import_path[1:]))
    except Exception as exception:
        raise CLIException("Unable to get module: {error}"
                           .format(error=str(exception)))

    return module


def completions(comp_words, current_word, argv):
    """
    Helps autocomplete by returning the appropriate
    completion words under three conditions.

    1) Returns `comp_words` if the completion word is
       potentially in that list.
    2) Returns an empty list if there is no possible
       completion.
    3) Returns `None` if the autocomplete is already done.
    """
    comp_words += ["-h", "--help", "--version"]

    if len(argv) == 0:
        return comp_words

    if len(argv) == 1:
        if argv[0] not in comp_words and current_word:
            return comp_words

        if argv[0] in comp_words and current_word:
            return comp_words

        if argv[0] not in comp_words and not current_word:
            return []

        if argv[0] in comp_words and not current_word:
            return None

    if len(argv) > 1 and argv[0] not in comp_words:
        return []

    if len(argv) > 1 and argv[0] in comp_words:
        return None

    raise CLIException("Unreachable")


def format_commands_help(cmds):
    """
    Helps format plugin commands for display.
    """
    longest_cmd_name = max(cmds.keys(), key=len)

    help_string = ""
    for cmd in sorted(cmds.keys()):
        # For the top-level entry point, `cmds` is a single-level
        # dictionary with `short_help` as the values. For plugins,
        # `cmds` is a two-level dictionary, where `short_help` is a
        # field in each sub-dictionary.
        short_help = cmds[cmd]
        if isinstance(short_help, dict):
            short_help = short_help["short_help"]

        num_spaces = len(longest_cmd_name) - len(cmd) + 2
        help_string += "  %s%s%s\n" % (cmd, " " * num_spaces, short_help)

    return help_string


def format_subcommands_help(cmd):
    """
    Helps format plugin subcommands for display.
    """
    arguments = " ".join(cmd["arguments"])
    short_help = cmd["short_help"]
    long_help = textwrap.dedent(cmd["long_help"].rstrip())
    long_help = "  " + "\n  ".join(long_help.split('\n'))
    flags = cmd["flags"]
    flags["-h --help"] = "Show this screen."
    flag_string = ""

    if len(flags.keys()) != 0:
        longest_flag_name = max(flags.keys(), key=len)
        for flag in sorted(flags.keys()):
            num_spaces = len(longest_flag_name) - len(flag) + 2
            flag_string += "  %s%s%s\n" % (flag, " " * num_spaces, flags[flag])

    return (arguments, short_help, long_help, flag_string)


def verify_address_format(address):
    """
    Verify that an address ip and port are correct.
    """
    # We use 'basestring' as the type of address because it can be
    # 'str' or 'unicode' depending on the source of the address (e.g.
    # a config file or a flag). Both types inherit from basestring.
    if not isinstance(address, basestring):
        raise CLIException("The address must be a string")

    address_pattern = re.compile(r'[0-9]+(?:\.[0-9]+){3}:[0-9]+')
    if not address_pattern.match(address):
        raise CLIException("The address '{address}' does not match"
                           " the expected format '<ip>:<port>'"
                           .format(address=address))

    colon_pos = address.rfind(':')
    ip = address[:colon_pos]
    port = int(address[colon_pos+1:])

    try:
        socket.inet_aton(ip)
    except socket.error as err:
        raise CLIException("The IP '{ip}' is not valid: {error}"
                           .format(ip=ip, error=err))

    # A correct port number is between these two values.
    if port < 0 or port > 65535:
        raise CLIException("The port '{port}' is not valid")


def get_agent_address(agent_id, master):
    """
    Given a master and an agent id, return the agent address
    by checking the /slaves endpoint of the master.
    """
    try:
        agents = http.get_json(master, "slaves")["slaves"]
    except Exception as exception:
        raise CLIException("Could not open '/slaves'"
                           " endpoint at '{addr}': {error}"
                           .format(addr=master,
                                   error=exception))
    for agent in agents:
        if agent["id"] == agent_id:
            return agent["pid"].split("@")[1]
    raise CLIException("Unable to find agent '{id}'".format(id=agent_id))


def join_plugin_paths(settings, config):
    """
    Return all the plugin paths combined
    from both settings and the config file.
    """
    builtin_paths = settings.PLUGINS

    try:
        config_paths = config.plugins()
    except Exception as exception:
        raise CLIException("Error: {error}.".format(error=str(exception)))

    return builtin_paths + config_paths


# TODO(agrillet): Implement this function appropriately (MESOS-8012).
def zookeeper_resolve_leader(addresses, path):
    """Resolve the leader using a znode path."""
    # pylint: disable=unused-argument, unreachable
    raise CLIException("Using ZooKeeper to resolve the leading master"
                       " is not yet supported. See MESOS-8012")
    return ""


class Table(object):
    """
    Defines a custom table structure for printing to the terminal.
    """
    def __init__(self, columns):
        """
        Initialize a table with a list of column names
        to act as headers for each column in the table.
        """
        if not isinstance(columns, list):
            raise CLIException("Column headers must be supplied as a list")

        for column in columns:
            if re.search(r"(\s)\1{2,}", column):
                raise CLIException("Column headers cannot have more"
                                   " than one space between words")

        self.table = [columns]
        self.padding = [len(column) for column in columns]

    def __getitem__(self, index):
        return list(self.table[index])

    def dimensions(self):
        """
        Returns the dimensions of the table as (<num-rows>, <num-columns>).
        """
        return (len(self.table), len(self.table[0]))

    def add_row(self, row):
        """
        Add a row to the table. Input must be a list where each entry
        corresponds to its respective column in order.
        """
        if len(row) != len(self.table[0]):
            raise CLIException("Number of entries and columns do not match!")

        # Adjust padding for each column.
        for index, elem in enumerate(row):
            if len(elem) > self.padding[index]:
                self.padding[index] = len(elem)

        self.table.append(row)

    def __str__(self):
        """
        Convert a table to string for printing.
        """
        table_string = ""
        for r_index, row in enumerate(self.table):
            for index, entry in enumerate(row):
                table_string += "%s%s" % \
                        (entry, " " * (self.padding[index] - len(entry) + 2))

            if r_index != len(self.table) - 1:
                table_string += "\n"

        return table_string

    @staticmethod
    def parse(string):
        """
        Parse a string previously printed as a `Table` back into a `Table`.
        """
        lines = string.split("\n")

        # Find the location and contents of column headers in the string.
        # Assume only single spaces between words in column headers.
        matches = re.finditer(r"([\w\d]+\s?[\w\d]+)+", lines[0])
        columns = [(m.start(), m.group()) for m in matches]

        # Build a table from the column header contents.
        table = Table([c[1] for c in columns])

        # Fill in the subsequent rows.
        for line in lines[1:]:
            row = []
            start_indices = [c[0] for c in columns]

            for i, start_index in enumerate(start_indices):
                if i + 1 < len(start_indices):
                    column = line[start_index:start_indices[i + 1]]
                else:
                    column = line[start_index:]

                row.append(str(column.strip()))

            table.add_row(row)

        return table
