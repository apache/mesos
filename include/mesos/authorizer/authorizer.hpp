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

#ifndef __MESOS_AUTHORIZER_AUTHORIZER_HPP__
#define __MESOS_AUTHORIZER_AUTHORIZER_HPP__

#include <mesos/mesos.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/authorizer/authorizer.pb.h>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {

class ACLs;

/**
 * This interface represents a function object returned by the
 * authorizer which can be used locally (and synchronously) to
 * check whether a specific object is authorized.
 */
class ObjectApprover
{
public:
  // This object has a 1:1 relationship with `authorization::Object`.
  // We need to ensure that the fields in this object are in sync
  // with the fields in `authorization::Object`.
  struct Object
  {
    Object()
      : value(nullptr),
        framework_info(nullptr),
        task(nullptr),
        task_info(nullptr),
        executor_info(nullptr),
        quota_info(nullptr),
        weight_info(nullptr),
        resource(nullptr),
        command_info(nullptr),
        container_id(nullptr) {}

    Object(const authorization::Object& object)
      : value(object.has_value() ? &object.value() : nullptr),
        framework_info(
            object.has_framework_info() ? &object.framework_info() : nullptr),
        task(object.has_task() ? &object.task() : nullptr),
        task_info(object.has_task_info() ? &object.task_info() : nullptr),
        executor_info(
            object.has_executor_info() ? &object.executor_info() : nullptr),
        quota_info(object.has_quota_info() ? &object.quota_info() : nullptr),
        weight_info(object.has_weight_info() ? &object.weight_info() : nullptr),
        resource(object.has_resource() ? &object.resource() : nullptr),
        command_info(
            object.has_command_info() ? &object.command_info() : nullptr),
        container_id(
            object.has_container_id() ? &object.container_id() : nullptr) {}

    const std::string* value;
    const FrameworkInfo* framework_info;
    const Task* task;
    const TaskInfo* task_info;
    const ExecutorInfo* executor_info;
    const quota::QuotaInfo* quota_info;
    const WeightInfo* weight_info;
    const Resource* resource;
    const CommandInfo* command_info;
    const ContainerID* container_id;
  };

  /**
   * NOTE: As this function can be used synchronously by actors
   * it is essential that it does not block!
   */
  virtual Try<bool> approved(
      const Option<Object>& object) const noexcept = 0;

  virtual ~ObjectApprover() = default;
};


/**
 * This interface is used to enable an identity service or any other
 * back end to check authorization policies for a set of predefined
 * actions.
 *
 * The `authorized()` method returns `Future<bool>`. If the action is
 * allowed, the future is set to `true`, otherwise to `false`. A third
 * possible outcome is that the future fails, which usually indicates
 * that the back end could not be contacted or it does not understand
 * the requested action. This may be a temporary condition.
 *
 * A description of the behavior of the default implementation of this
 * interface can be found in "docs/authorization.md".
 *
 * @see authorizer.proto
 */
class Authorizer
{
public:
  /**
   * Factory method used to create instances of authorizer which are loaded from
   * the `ModuleManager`. The parameters necessary to instantiate the authorizer
   * are taken from the contents of the `--modules` flag.
   *
   * @param name The name of the module to be loaded as registered in the
   *     `--modules` flag.
   *
   * @return An instance of `Authorizer*` if the module with the given name
   *     could be constructed. An error otherwise.
   */
  static Try<Authorizer*> create(const std::string &name);

  /**
   * Factory method used to create instances of the default 'local'  authorizer.
   *
   * @param acls The access control lists used to initialize the 'local'
   *     authorizer.
   *
   * @return An instance of the default 'local'  authorizer.
   */
  static Try<Authorizer*> create(const ACLs& acls);

  virtual ~Authorizer() {}

  /**
   * Checks with the identity server back end whether `request` is
   * allowed by the policies of the identity server, i.e. `request.subject`
   * can perform `request.action` with `request.object`. For details
   * on how the request is built and what its parts are, refer to
   * "authorizer.proto".
   *
   * @param request `authorization::Request` instance packing all the
   *     parameters needed to verify whether a subject can perform
   *     a given action with an object.
   *
   * @return `true` if the action is allowed, the future is set to `true`,
   *     otherwise `false`. A failed future indicates a problem processing
   *     the request, and it might be retried in the future.
   */
  virtual process::Future<bool> authorized(
      const authorization::Request& request) = 0;

  /**
   * Creates an `ObjectApprover` which can synchronously check authorization on
   * an object.
   *
   * @param subject `authorization::Subject` subject for which the
   *     `ObjectApprover` should be created.
   *
   * @param action `authorization::Action` action for which the
   *     `ObjectApprover` should be created.
   *
   * @return An `ObjectApprover` for the given `subject` and `action`.
   */
  virtual process::Future<process::Owned<ObjectApprover>> getObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action) = 0;

protected:
  Authorizer() {}
};

} // namespace mesos {

#endif // __MESOS_AUTHORIZER_AUTHORIZER_HPP__
