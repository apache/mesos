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
This is the main executable of the mesos-cli unit tests.
"""

import os
import unittest

from cli.tests import CLITestCase

# pylint: disable=unused-import
# We import the tests that we want to run.
from cli.tests import TestAgentPlugin
from cli.tests import TestInfrastructure
from cli.tests import TestTaskPlugin

if __name__ == '__main__':
    CLITestCase.MESOS_BUILD_DIR = CLITestCase.default_mesos_build_dir()
    os.environ["MESOS_CLI_CONFIG"] = os.path.join(os.path.dirname(__file__),
                                                  "default_config.toml")

    unittest.main(verbosity=2, testRunner=unittest.TextTestRunner)
