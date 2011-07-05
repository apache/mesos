/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "tests/external_test.hpp"


// Run a number of tests for the LXC isolation module.
// These tests are disabled by default since they require alltests to be run
// with sudo for Linux Container commands to be usable (and of course, they
// also require a Linux version with LXC support).
// You can enable them using ./alltests --gtest_also_run_disabled_tests.
TEST_EXTERNAL(LxcIsolation, DISABLED_TwoSeparateTasks)
TEST_EXTERNAL(LxcIsolation, DISABLED_ScaleUpAndDown)
TEST_EXTERNAL(LxcIsolation, DISABLED_HoldMoreMemThanRequested)
