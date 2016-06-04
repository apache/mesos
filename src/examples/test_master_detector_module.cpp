// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/module/detector.hpp>

#include <stout/try.hpp>

#include "master/detector/standalone.hpp"

using namespace mesos;
using namespace mesos::master;
using namespace mesos::master::detector;

// Declares a MasterDetector module named
// 'org_apache_mesos_TestMasterDetector'.
mesos::modules::Module<MasterDetector> org_apache_mesos_TestMasterDetector(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test MasterDetector module.",
    nullptr,
    [](const Parameters& parameters) -> MasterDetector* {
      return new StandaloneMasterDetector();
    });
