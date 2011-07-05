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

#include "isolation_module_factory.hpp"
#include "process_based_isolation_module.hpp"
#ifdef __sun__
#include "solaris_project_isolation_module.hpp"
#elif __linux__
#include "lxc_isolation_module.hpp"
#endif

using namespace mesos::internal::slave;


DEFINE_FACTORY(IsolationModule, Slave *)
{
  registerClass<ProcessBasedIsolationModule>("process");
#ifdef __sun__
  registerClass<SolarisProjectIsolationModule>("project");
#elif __linux__
  registerClass<LxcIsolationModule>("lxc");
#endif
}
