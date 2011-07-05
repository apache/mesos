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

#ifndef SLAVE_STATE_HPP
#define SLAVE_STATE_HPP

#include <iostream>
#include <string>
#include <vector>

#include "common/foreach.hpp"

#include "config/config.hpp"


namespace mesos { namespace internal { namespace slave { namespace state {

struct Task
{
  Task(std::string id_, const std::string& name_, std::string state_,
      double cpus_, double mem_)
    : id(id_), name(name_), state(state_), cpus(cpus_), mem(mem_) {}

  Task() {}

  std::string id;
  std::string name;
  std::string state;
  double cpus;
  double mem;
};

struct Framework
{
  Framework(std::string id_, const std::string& name_,
      const std::string& executor_uri_, const std::string& executor_status_,
      double cpus_, double mem_)
    : id(id_), name(name_), executor_uri(executor_uri_),
      executor_status(executor_status_), cpus(cpus_), mem(mem_) {}

  Framework() {}

  ~Framework()
  {
    foreach (Task *task, tasks)
      delete task;
  }

  std::string id;
  std::string name;
  std::string executor_uri;
  std::string executor_status;
  double cpus;
  double mem;

  std::vector<Task *> tasks;
};

struct SlaveState
{
  SlaveState(const std::string& build_date_, const std::string& build_user_,
	     std::string id_, double cpus_, double mem_, const std::string& pid_,
	     const std::string& master_pid_)
    : build_date(build_date_), build_user(build_user_), id(id_),
      cpus(cpus_), mem(mem_), pid(pid_), master_pid(master_pid_) {}

  SlaveState() {}

  ~SlaveState()
  {
    foreach (Framework *framework, frameworks)
      delete framework;
  }

  std::string build_date;
  std::string build_user;
  std::string id;
  double cpus;
  double mem;
  std::string pid;
  std::string master_pid;

  std::vector<Framework *> frameworks;
};

}}}} /* namespace */

#endif /* SLAVE_STATE_HPP */
