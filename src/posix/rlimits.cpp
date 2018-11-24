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

#include <sys/time.h>
#include <sys/types.h>

// Intentionally include `sys/resource.h` after `sys/time.h` and
// `sys/types.h`. This is the suggested include pattern under e.g.,
// BSD, https://www.freebsd.org/cgi/man.cgi?query=setrlimit.
#include <sys/resource.h>

#include <stout/os/strerror.hpp>

#include <stout/unreachable.hpp>

#include "posix/rlimits.hpp"

namespace mesos {
namespace internal {
namespace rlimits {

Try<int> convert(RLimitInfo::RLimit::Type type)
{
  const Error error(
      "Resource type '" + RLimitInfo_RLimit_Type_Name(type) +
      "' not supported");

  switch (type) {
    // Resource types defined in XSI.
    case RLimitInfo::RLimit::RLMT_AS:       return RLIMIT_AS;
    case RLimitInfo::RLimit::RLMT_CORE:     return RLIMIT_CORE;
    case RLimitInfo::RLimit::RLMT_CPU:      return RLIMIT_CPU;
    case RLimitInfo::RLimit::RLMT_DATA:     return RLIMIT_DATA;
    case RLimitInfo::RLimit::RLMT_FSIZE:    return RLIMIT_FSIZE;
    case RLimitInfo::RLimit::RLMT_NOFILE:   return RLIMIT_NOFILE;
    case RLimitInfo::RLimit::RLMT_STACK:    return RLIMIT_STACK;

    // Resource types also defined on BSDs like e.g., OS X.
    case RLimitInfo::RLimit::RLMT_MEMLOCK:  return RLIMIT_MEMLOCK;
    case RLimitInfo::RLimit::RLMT_NPROC:    return RLIMIT_NPROC;
    case RLimitInfo::RLimit::RLMT_RSS:      return RLIMIT_RSS;

    // Resource types defined in >=Linux 2.6.36.
    // NOTE: The resource limits defined for Linux are currently the
    // maximal possible set of understood types. Here we explicitly
    // list all types and in particular do not use a `default` case,
    // see MESOS-3754.
    case RLimitInfo::RLimit::RLMT_LOCKS:
#ifdef RLIMIT_LOCKS
      return RLIMIT_LOCKS;
#else
      return error;
#endif

    case RLimitInfo::RLimit::RLMT_MSGQUEUE:
#ifdef RLIMIT_MSGQUEUE
      return RLIMIT_MSGQUEUE;
#else
      return error;
#endif

    case RLimitInfo::RLimit::RLMT_NICE:
#ifdef RLIMIT_NICE
      return RLIMIT_NICE;
#else
      return error;
#endif

    case RLimitInfo::RLimit::RLMT_RTPRIO:
#ifdef RLIMIT_RTPRIO
      return RLIMIT_RTPRIO;
#else
      return error;
#endif

    case RLimitInfo::RLimit::RLMT_RTTIME:
#ifdef RLIMIT_RTTIME
      return RLIMIT_RTTIME;
#else
      return error;
#endif

    case RLimitInfo::RLimit::RLMT_SIGPENDING:
#ifdef RLIMIT_SIGPENDING
      return RLIMIT_SIGPENDING;
#else
      return error;
#endif

    case RLimitInfo::RLimit::UNKNOWN:
      return Error("Unknown rlimit type");
  }

  UNREACHABLE();
}


Try<Nothing> set(const RLimitInfo::RLimit& limit)
{
  const Try<int> resource = convert(limit.type());
  if (resource.isError()) {
    return Error("Could not convert rlimit: " + resource.error());
  }

  ::rlimit resourceLimit;
  if (limit.has_soft() && limit.has_hard()) {
    resourceLimit.rlim_cur = limit.soft();
    resourceLimit.rlim_max = limit.hard();
  } else if (!limit.has_soft() && !limit.has_hard()) {
    resourceLimit.rlim_cur = RLIM_INFINITY;
    resourceLimit.rlim_max = RLIM_INFINITY;
  } else {
    return Error("Invalid rlimit values");
  }

  if (setrlimit(resource.get(), &resourceLimit) != 0) {
    return ErrnoError();
  }

  return Nothing();
}


Try<RLimitInfo::RLimit> get(RLimitInfo::RLimit::Type type)
{
  Try<int> _type = rlimits::convert(type);
  if (_type.isError()) {
    return Error(_type.error());
  }

  ::rlimit resourceLimit;
  if (getrlimit(_type.get(), &resourceLimit) != 0) {
    return ErrnoError();
  }

  RLimitInfo::RLimit limit;
  limit.set_type(type);
  if (resourceLimit.rlim_cur != RLIM_INFINITY) {
    limit.set_soft(resourceLimit.rlim_cur);
  }
  if (resourceLimit.rlim_max != RLIM_INFINITY) {
    limit.set_hard(resourceLimit.rlim_max);
  }

  return std::move(limit);
}

} // namespace rlimits {
} // namespace internal {
} // namespace mesos {
