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

%module(directors="1") master

#define SWIG_NO_EXPORT_ITERATOR_METHODS

%{
#include <master/state.hpp>

namespace mesos { namespace internal { namespace master { namespace state {
extern MasterState *get_master();
}}}}

#define SWIG_STD_NOASSIGN_STL
%}

%include <stdint.i>
%include <std_string.i>
%include <std_vector.i>

%template(SlaveVec) std::vector<mesos::internal::master::state::Slave*>;
%template(FrameworkVec) std::vector<mesos::internal::master::state::Framework*>;
%template(TaskVec) std::vector<mesos::internal::master::state::Task*>;
%template(OfferVec) std::vector<mesos::internal::master::state::Offer*>;
%template(SlaveResourcesVec) std::vector<mesos::internal::master::state::SlaveResources*>;

%include <master/state.hpp>

namespace mesos { namespace internal { namespace master { namespace state {
%newobject get_master;
extern MasterState *get_master();
}}}}

