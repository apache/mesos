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
This is the main executable of the mesos-cli.
"""

import sys

import settings

import cli

from cli.docopt import docopt
from cli.exceptions import CLIException


VERSION = "Mesos " + settings.VERSION + " CLI"

SHORT_HELP = "Perform operations on a running Mesos cluster."

USAGE = \
"""Mesos CLI

Usage:
  mesos (-h | --help)
  mesos --version
  mesos <command> [<args>...]

Options:
  -h --help  Show this screen.
  --version  Show version info.

Commands:
{commands}
See 'mesos help <command>' for more information on a specific command.
"""


def autocomplete(cmds, plugins, config, argv):
    """
    Perform autocomplete for the given input arguments. If not
    completing a top level command (or "help"), this function passes
    the arguments down to the appropriate plugins for per-plugin
    autocompletion.
    """

    option = "default"
    current_word = argv[0]
    argv = argv[1:]

    if argv and argv[0] == "help":
        argv = argv[1:]

    comp_words = list(cmds.keys()) + ["help"]
    comp_words = cli.util.completions(comp_words, current_word, argv)
    if comp_words is not None:
        return (option, comp_words)

    plugin = cli.util.get_module(plugins, argv[0])
    plugin_class = getattr(plugin, plugin.PLUGIN_CLASS)

    return plugin_class(settings, config).__autocomplete_base__(
        current_word,
        argv[1:])


def main(argv):
    """
    This is the main function for the Mesos CLI.
    """

    # Load the CLI config.
    config = cli.config.Config(settings)

    plugins = cli.util.import_modules(
        cli.util.join_plugin_paths(settings, config),
        "plugins")

    cmds = {
        cli.util.get_module(plugins, plugin).PLUGIN_NAME:
        cli.util.get_module(plugins, plugin).SHORT_HELP
        for plugin in list(plugins.keys())
    }

    # Parse all incoming arguments using docopt.
    command_strings = ""
    if cmds != {}:
        command_strings = cli.util.format_commands_help(cmds)
    usage = USAGE.format(commands=command_strings)

    arguments = docopt(usage, argv=argv, version=VERSION, options_first=True)

    cmd = arguments["<command>"]
    argv = arguments["<args>"]

    # Use the meta-command `__autocomplete__` to perform
    # autocompletion on the remaining arguments.
    if cmd == "__autocomplete__":
        option = "default"
        comp_words = []

        # If there is an error performing the autocomplete, treat it
        # as if we were just unable to complete any words. This avoids
        # passing the erroring stack trace back as the list of words
        # to complete on.
        try:
            option, comp_words = autocomplete(cmds, plugins, config, argv)
        except Exception:
            pass

        print(option)
        print(" ".join(comp_words))
        return 0

    # Use the meta-command "help" to print help information for the
    # supplied command and its subcommands.
    if cmd == "help":
        if argv and argv[0] in cmds:
            plugin = cli.util.get_module(plugins, argv[0])
            plugin_class = getattr(plugin, plugin.PLUGIN_CLASS)
            return plugin_class(settings, config).main(argv[1:] + ["--help"])

        return main(["--help"])

    # Run the command through its plugin.
    if cmd in list(cmds.keys()):
        plugin = cli.util.get_module(plugins, cmd)
        plugin_class = getattr(plugin, plugin.PLUGIN_CLASS)
        return plugin_class(settings, config).main(argv)

    # Print help information if no commands match.
    return main(["--help"])


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except CLIException as exception:
        sys.exit("Error: {error}.".format(error=str(exception)))
    except KeyboardInterrupt:
        pass
