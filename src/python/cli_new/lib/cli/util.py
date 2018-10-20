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
import ipaddress
import json
import os
import re
import textwrap
import urllib.parse

from kazoo.client import KazooClient

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

    if not argv:
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
    longest_cmd_name = max(list(cmds.keys()), key=len)

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
    long_help = "  " + "\n  ".join(long_help.lstrip().split('\n'))
    flags = cmd["flags"]
    flags["-h --help"] = "Show this screen."
    flag_string = ""

    if list(flags.keys()) != 0:
        longest_flag_name = max(list(flags.keys()), key=len)
        for flag in sorted(flags.keys()):
            num_spaces = len(longest_flag_name) - len(flag) + 2
            flag_string += "  %s%s%s\n" % (flag, " " * num_spaces, flags[flag])
    flag_string = flag_string.rstrip()

    return (arguments, short_help, long_help, flag_string)


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


def sanitize_address(address):
    """
    Sanitize an address, ensuring that it has a format recognizable by the CLI.
    """
    # Try and parse the address to make sure it is parseable.
    try:
        parsed = urllib.parse.urlparse(address)
    except Exception as exception:
        raise CLIException("Unable to parse address: {error}"
                           .format(error=str(exception)))

    # Since we allow addresses to be specified without an
    # explicit scheme, some fields in the parsed address may
    # be missing. Patch it up to force an implicit HTTP scheme.
    if parsed.scheme == "" and parsed.netloc == "":
        address = "http://{addr}".format(addr=address)
    elif parsed.scheme == "" and parsed.netloc != "":
        address = "http:{addr}".format(addr=address)

    # Try and parse the address again to make sure it
    # now has all the parts we expect and that they are valid.
    try:
        parsed = urllib.parse.urlparse(address)
    except Exception as exception:
        raise CLIException("Unable to parse address: {error}"
                           .format(error=str(exception)))

    # We only support HTTP and HTTPS schemes.
    if parsed.scheme != "http" and parsed.scheme != "https":
        raise CLIException("Invalid scheme '{scheme}' in address"
                           .format(scheme=parsed.scheme))

    # There must be a hostname present.
    if parsed.hostname == "":
        raise CLIException("Missing hostname in address")

    # We do not support IPv6 in the hostname (yet).
    try:
        ipaddress.IPv6Address(parsed.hostname)
        raise CLIException("IPv6 addresses are unsupported")
    except Exception as exception:
        pass

    valid_ip_v4_address = False

    # We either accept IPv4 addresses, or DNS names as the hostname. In the
    # check below we try and parse the hostname as an IPv4 address, if this
    # does not succeed, then we assume the hostname is formatted as a DNS name.
    try:
        ipaddress.IPv4Address(parsed.hostname)
        valid_ip_v4_address = True
    except Exception as exception:
        pass

    # If we have an IPv4 address then we require a port to be specified.
    if valid_ip_v4_address and parsed.port is None:
        raise CLIException("Addresses formatted as IP must contain a port")

    # We allow ports for both IPv4 addresses and DNS
    # names, but they must be in a specific range.
    if parsed.port and (parsed.port < 0 or parsed.port > 65535):
        raise CLIException("Port '{port}' is out of range"
                           .format(port=parsed.port))

    return address


def zookeeper_resolve_leader(addresses, path):
    """
    Resolve the leader using a znode path. ZooKeeper imposes a total
    order on the elements of the queue, guaranteeing that the
    oldest element of the queue is the first one. We can
    thus return the first address we get from ZooKeeper.
    """
    hosts = ",".join(addresses)

    try:
        zk = KazooClient(hosts=hosts)
        zk.start()
    except Exception as exception:
        raise CLIException("Unable to initialize Zookeeper Client: {error}"
                           .format(error=exception))

    try:
        children = zk.get_children(path)
    except Exception as exception:
        raise CLIException("Unable to get children of {zk_path}: {error}"
                           .format(zk_path=path, error=exception))

    masters = sorted(
        # 'json.info' is the prefix for master nodes.
        child for child in children if child.startswith("json.info")
    )

    address = ""
    for master in masters:
        try:
            node_path = "{path}/{node}".format(path=path, node=master)
            json_data, _ = zk.get(node_path)
        except Exception as exception:
            raise CLIException("Unable to get the value of '{node}': {error}"
                               .format(node=node_path, error=exception))

        try:
            data = json.loads(json_data)
        except Exception as exception:
            raise CLIException("Could not load JSON from '{data}': {error}"
                               .format(data=data, error=str(exception)))

        if ("address" in data and "ip" in data["address"] and
                "port" in data["address"]):
            address = "{ip}:{port}".format(ip=data["address"]["ip"],
                                           port=data["address"]["port"])
            break

    try:
        zk.stop()
    except Exception as exception:
        raise CLIException("Unable to stop Zookeeper Client: {error}"
                           .format(error=exception))

    if not address:
        raise CLIException("Unable to resolve the leading"
                           " master using ZooKeeper")
    return address


class Table():
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
