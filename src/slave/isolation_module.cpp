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

#include "isolation_module.hpp"
#include "process_based_isolation_module.hpp"
#ifdef __sun__
#include "solaris_project_isolation_module.hpp"
#elif __linux__
#include "cgroups_isolation_module.hpp"
#include "lxc_isolation_module.hpp"
#endif


namespace mesos { namespace internal { namespace slave {

IsolationModule* IsolationModule::create(const std::string &type)
{
  if (type == "process")
    return new ProcessBasedIsolationModule();
#ifdef __sun__
  else if (type == "project")
    return new SolarisProjectIsolationModule();
#elif __linux__
  else if (type == "cgroups")
    return new CgroupsIsolationModule();
  else if (type == "lxc")
    return new LxcIsolationModule();
#endif

  return NULL;
}


void IsolationModule::destroy(IsolationModule* module)
{
  if (module != NULL) {
    delete module;
  }
}

}}} // namespace mesos { namespace internal { namespace slave {
