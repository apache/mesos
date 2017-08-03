// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __PROCESS_CHECK_HPP__
#define __PROCESS_CHECK_HPP__

#include <string>

#include <stout/check.hpp>
#include <stout/option.hpp>

#include <process/future.hpp>

// Provides a CHECK_PENDING macro, akin to CHECK.
// This appends the error if possible to the end of the log message, so there's
// no need to append the error message explicitly.
#define CHECK_PENDING(expression)                                       \
  CHECK_STATE(CHECK_PENDING, _check_pending, expression)

#define CHECK_READY(expression)                                         \
  CHECK_STATE(CHECK_READY, _check_ready, expression)

#define CHECK_DISCARDED(expression)                                     \
  CHECK_STATE(CHECK_DISCARDED, _check_discarded, expression)

#define CHECK_FAILED(expression)                                        \
  CHECK_STATE(CHECK_FAILED, _check_failed, expression)

#define CHECK_ABANDONED(expression)                                     \
  CHECK_STATE(CHECK_ABANDONED, _check_abandoned, expression)

// Private structs/functions used for CHECK_*.

template <typename T>
Option<Error> _check_pending(const process::Future<T>& f)
{
  if (f.isReady()) {
    return Error("is READY");
  } else if (f.isDiscarded()) {
    return Error("is DISCARDED");
  } else if (f.isFailed()) {
    return Error("is FAILED: " + f.failure());
  } else {
    CHECK(f.isPending());
    return None();
  }
}


template <typename T>
Option<Error> _check_ready(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Error("is PENDING");
  } else if (f.isDiscarded()) {
    return Error("is DISCARDED");
  } else if (f.isFailed()) {
    return Error("is FAILED: " + f.failure());
  } else {
    CHECK(f.isReady());
    return None();
  }
}


template <typename T>
Option<Error> _check_discarded(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Error("is PENDING");
  } else if (f.isReady()) {
    return Error("is READY");
  } else if (f.isFailed()) {
    return Error("is FAILED: " + f.failure());
  } else {
    CHECK(f.isDiscarded());
    return None();
  }
}


template <typename T>
Option<Error> _check_failed(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Some("is PENDING");
  } else if (f.isReady()) {
    return Some("is READY");
  } else if (f.isDiscarded()) {
    return Some("is DISCARDED");
  } else {
    CHECK(f.isFailed());
    return None();
  }
}


template <typename T>
Option<Error> _check_abandoned(const process::Future<T>& f)
{
  if (f.isReady()) {
    return Some("is READY");
  } else if (f.isDiscarded()) {
    return Some("is DISCARDED");
  } else if (f.isFailed()) {
    return Some("is FAILED: " + f.failure());
  } else if (!f.isAbandoned()) {
    return Some("is not abandoned");
  }
  return None();
}

// TODO(dhamon): CHECK_NPENDING, CHECK_NREADY, etc.

#endif // __PROCESS_CHECK_HPP__
