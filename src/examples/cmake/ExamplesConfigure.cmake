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

# Define test module library targets.
#####################################
# These libraries are usually the default implementation
# of the module pulled out into a shared library.
set(TEST_ALLOCATOR          testallocator)
set(TEST_ANONYMOUS          testanonymous)
set(TEST_AUTHENTICATION     testauthentication)
set(TEST_AUTHORIZER         testauthorizer)
set(TEST_CONTAINER_LOGGER   testcontainer_logger)
set(TEST_EXAMPLEMODULE      examplemodule)
set(TEST_HOOK               testhook)
set(TEST_HTTPAUTHENTICATOR  testhttpauthenticator)
set(TEST_ISOLATOR           testisolator)
set(TEST_MASTER_CONTENDER   testmastercontender)
set(TEST_MASTER_DETECTOR    testmasterdetector)
set(TEST_QOS_CONTROLLER     testqos_controller)
set(TEST_RESOURCE_ESTIMATOR testresource_estimator)


# Define example framework and executor targets.
################################################
set(BALLOON_EXECUTOR              balloon-executor)
set(BALLOON_FRAMEWORK             balloon-framework)
set(DISK_FULL_FRAMEWORK           disk-full-framework)
set(DOCKER_NO_EXECUTOR_FRAMEWORK  docker-no-executor-framework)
set(DYNAMIC_RESERVATION_FRAMEWORK dynamic-reservation-framework)
set(LOAD_GENERATOR_FRAMEWORK      load-generator-framework)
set(LONG_LIVED_EXECUTOR           long-lived-executor)
set(LONG_LIVED_FRAMEWORK          long-lived-framework)
set(NO_EXECUTOR_FRAMEWORK         no-executor-framework)
set(PERSISTENT_VOLUME_FRAMEWORK   persistent-volume-framework)
set(TEST_EXECUTOR                 test-executor)
set(TEST_FRAMEWORK                test-framework)
set(TEST_HTTP_EXECUTOR            test-http-executor)
set(TEST_HTTP_FRAMEWORK           test-http-framework)
