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

#ifndef __TESTS_MODULE_HPP__
#define __TESTS_MODULE_HPP__

#include <string>

#include <mesos/module/module.hpp>

#include <stout/try.hpp>

#include "logging/flags.hpp"
#include "messages/messages.hpp"
#include "module/manager.hpp"

#include "tests/mesos.hpp"

namespace mesos {
namespace tests {

// The ModuleID is used by typed tests to specify the specific module
// name for the test instance.  Ideally, we would have passed the
// module name itself, but templates do not allow string literals as
// template parameters.
enum ModuleID
{
  TestMemIsolator,
  TestCpuIsolator,
  TestCRAMMD5Authenticatee,
  TestCRAMMD5Authenticator,
  TestHook
};


Try<Nothing> initModules(const Option<Modules>& modules);

Try<std::string> getModuleName(ModuleID id);


template <typename T, ModuleID N>
class Module
{
public:
  // Create is used by the type_param'ed tests.  T here denotes the
  // module type, whereas N denotes the module name.
  static Try<T*> create()
  {
    Try<std::string> moduleName = getModuleName(N);
    if (moduleName.isError()) {
      return Error(moduleName.error());
    }
    return mesos::modules::ModuleManager::create<T>(moduleName.get());
  }

  static Try<T*> create(logging::Flags flags)
  {
    return create();
  }
};

} // namespace tests {
} // namespace mesos {

#endif // __TESTS_MODULE_HPP__
