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

#include <ostream>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include "authorizer/local/authorizer.hpp"

#include "master/constants.hpp"

#include "module/manager.hpp"

using std::ostream;
using std::string;

using mesos::internal::LocalAuthorizer;

namespace mesos {

Try<Authorizer*> Authorizer::create(const string& name)
{
  // Create an instance of the default authorizer. If other than the
  // default authorizer is requested, search for it in loaded modules.
  // NOTE: We do not need an extra not-null check, because both
  // ModuleManager and built-in authorizer factory do that already.
  if (name == mesos::internal::master::DEFAULT_AUTHORIZER) {
    return LocalAuthorizer::create();
  }

  return modules::ModuleManager::create<Authorizer>(name);
}


ostream& operator<<(ostream& stream, const ACLs& acls)
{
  return stream << acls.DebugString();
}

} // namespace mesos {
