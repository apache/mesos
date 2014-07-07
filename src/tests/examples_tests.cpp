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

#include "tests/script.hpp"


// Run each of the sample frameworks in local mode.
TEST_SCRIPT(ExamplesTest, TestFramework, "test_framework_test.sh")
TEST_SCRIPT(ExamplesTest, NoExecutorFramework, "no_executor_framework_test.sh")
TEST_SCRIPT(ExamplesTest, LowLevelSchedulerLibprocess,
            "low_level_scheduler_libprocess_test.sh")

#ifdef MESOS_HAS_JAVA
TEST_SCRIPT(ExamplesTest, JavaFramework, "java_framework_test.sh")
TEST_SCRIPT(ExamplesTest, JavaException, "java_exception_test.sh")
TEST_SCRIPT(ExamplesTest, JavaLog, "java_log_test.sh")
#endif

#ifdef MESOS_HAS_PYTHON
TEST_SCRIPT(ExamplesTest, PythonFramework, "python_framework_test.sh")
#endif
