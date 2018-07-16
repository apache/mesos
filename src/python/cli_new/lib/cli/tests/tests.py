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
Tests that test the test infrastructure itself.
"""

from cli.tests import capture_output
from cli.tests import CLITestCase
from cli.tests import Agent
from cli.tests import Master
from cli.tests import Task


class TestInfrastructure(CLITestCase):
    """
    This class tests the functionality of the test
    infrastructure used to run all other CLI tests.
    """
    def test_launch_binaries(self):
        """
        Tests the launching and killing of
        a mesos master, agent, and task.
        """
        master = Master()
        master.launch()

        agent = Agent()
        agent.launch()

        task = Task({"command": "sleep 1000"})
        task.launch()

        task.kill()
        agent.kill()
        master.kill()

    def test_capture_output(self):
        """
        Tests the ability to capture the output
        from an arbitrary CLI sub-command.
        """
        template = "Arguments: {args}"
        arguments = ["argument1", "argument2"]

        # pylint: disable=missing-docstring
        def command(args):
            print(template.format(args=args))

        output = capture_output(command, arguments)

        self.assertEqual(output, template.format(args=arguments))
