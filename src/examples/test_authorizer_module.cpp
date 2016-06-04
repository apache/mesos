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

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/authorizer/acls.hpp>
#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "common/parse.hpp"

using std::string;

using namespace mesos;

static Authorizer* createLocalAuthorizer(const Parameters& parameters)
{
  Try<Authorizer*> local = mesos::internal::LocalAuthorizer::create(parameters);
  if (local.isError()) {
    return nullptr;
  }

  return local.get();
}


// Declares an Authorizer module named
// 'org_apache_mesos_TestLocalAuthorizer'.
mesos::modules::Module<Authorizer> org_apache_mesos_TestLocalAuthorizer(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test Authorizer module.",
    nullptr,
    createLocalAuthorizer);
