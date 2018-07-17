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

#ifndef __MESOS_CONTAINERIZER_MOUNT_HPP__
#define __MESOS_CONTAINERIZER_MOUNT_HPP__

#include <stout/option.hpp>
#include <stout/subcommand.hpp>

namespace mesos {
namespace internal {
namespace slave {

// "mount" subcommand functions similarly to the mount program.
// However, this subcommand is necessary because of the following reasons:
// - `mount --make-rslave <dir>` doesn't work on ubuntu 14.04 due to an existing
//    bug.
// - mount program only handles mounts that is recorded in /etc/mtab and
//   /etc/fstab, and ignores mounts that are done via mount syscall.
//   We need this subcommand so we can effect all mounts.
class MesosContainerizerMount : public Subcommand
{
public:
  static const std::string NAME;
  static const std::string MAKE_RSLAVE;

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<std::string> operation;
    Option<std::string> path;
  };

  MesosContainerizerMount() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_MOUNT_HPP__
