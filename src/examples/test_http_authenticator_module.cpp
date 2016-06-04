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

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/http_authenticator.hpp>

#include <process/authenticator.hpp>

#include <stout/hashmap.hpp>

#include "logging/logging.hpp"

using namespace mesos;

using mesos::http::authentication::BasicAuthenticatorFactory;

using process::http::authentication::Authenticator;


static Authenticator* createHttpAuthenticator(const Parameters& parameters)
{
  Try<Authenticator*> authenticator =
    BasicAuthenticatorFactory::create(parameters);

  if (authenticator.isError()) {
    LOG(ERROR) << "Failed to create basic HTTP authenticator: "
               << authenticator.error();

    return nullptr;
  }

  return authenticator.get();
}


// Declares an HTTP Authenticator module named
// 'org_apache_mesos_TestHttpBasicAuthenticator'.
mesos::modules::Module<Authenticator>
org_apache_mesos_TestHttpBasicAuthenticator(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test HTTP Authenticator module.",
    nullptr,
    createHttpAuthenticator);
