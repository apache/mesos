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

#include <mesos/authentication/authenticatee.hpp>
#include <mesos/authentication/authenticator.hpp>

#include <mesos/module/authenticatee.hpp>
#include <mesos/module/authenticator.hpp>

#include "authentication/cram_md5/authenticatee.hpp"
#include "authentication/cram_md5/authenticator.hpp"

using namespace mesos;

using mesos::Authenticatee;
using mesos::Authenticator;

static bool compatible()
{
  return true;
}


static Authenticatee* createCRAMMD5Authenticatee(const Parameters& parameters)
{
  return new mesos::internal::cram_md5::CRAMMD5Authenticatee();
}


mesos::modules::Module<Authenticatee> org_apache_mesos_TestCRAMMD5Authenticatee(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test CRAM-MD5 SASL authenticatee module.",
    compatible,
    createCRAMMD5Authenticatee);


static Authenticator* createCRAMMD5Authenticator(const Parameters& parameters)
{
  return new mesos::internal::cram_md5::CRAMMD5Authenticator();
}


mesos::modules::Module<Authenticator> org_apache_mesos_TestCRAMMD5Authenticator(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test CRAM-MD5 SASL authenticator module.",
    compatible,
    createCRAMMD5Authenticator);
