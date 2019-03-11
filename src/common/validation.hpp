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

#ifndef __COMMON_VALIDATION_HPP__
#define __COMMON_VALIDATION_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>

namespace mesos {

namespace executor {

class Call;

} // namespace executor {

namespace internal {
namespace common {
namespace validation {

// Validates if `id` meets the common requirements for IDs in Mesos.
// Note that some IDs in Mesos have additional/stricter requirements.
Option<Error> validateID(const std::string& id);

Option<Error> validateTaskID(const TaskID& taskId);

Option<Error> validateExecutorID(const ExecutorID& executorId);

Option<Error> validateSlaveID(const SlaveID& slaveId);

Option<Error> validateFrameworkID(const FrameworkID& frameworkId);

Option<Error> validateSecret(const Secret& secret);

Option<Error> validateEnvironment(const Environment& environment);

Option<Error> validateCommandInfo(const CommandInfo& command);

Option<Error> validateVolume(const Volume& volume);

Option<Error> validateContainerInfo(const ContainerInfo& containerInfo);

Option<Error> validateGpus(
    const google::protobuf::RepeatedPtrField<Resource>& resources);

Option<Error> validateHealthCheck(const HealthCheck& healthCheck);

Option<Error> validateCheckInfo(const CheckInfo& checkInfo);

Option<Error> validateCheckStatusInfo(const CheckStatusInfo& checkStatusInfo);

Option<Error> validateExecutorCall(const mesos::executor::Call& call);

Option<Error> validateOfferFilters(const OfferFilters& offerFilters);

Option<Error> validateInputScalarValue(double value);

} // namespace validation {
} // namespace common {
} // namespace internal {
} // namespace mesos {

#endif // __COMMON_VALIDATION_HPP__
