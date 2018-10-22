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
A collection of constants useful for the CLI unit tests.
"""

import os


TEST_MASTER_IP = "127.0.0.1"
TEST_MASTER_PORT = "9090"

TEST_AGENT_IP = "127.0.0.1"
TEST_AGENT_PORT = "9091"

TEST_DIRECTORY = os.path.dirname(os.path.realpath(__file__))
TEST_DATA_DIRECTORY = os.path.join(TEST_DIRECTORY, "data")
