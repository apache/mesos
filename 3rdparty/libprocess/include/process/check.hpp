/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
  for (const Option<std::string> _error = _checkPending(expression);    \
       _error.isSome();)                                                \
    _CheckFatal(__FILE__, __LINE__, "CHECK_PENDING",                    \
                #expression, _error.get()).stream()

#define CHECK_READY(expression)                                         \
  for (const Option<std::string> _error = _checkReady(expression);      \
       _error.isSome();)                                                \
    _CheckFatal(__FILE__, __LINE__, "CHECK_READY",                      \
                #expression, _error.get()).stream()

#define CHECK_DISCARDED(expression)                                     \
  for (const Option<std::string> _error = _checkDiscarded(expression);  \
       _error.isSome();)                                                \
    _CheckFatal(__FILE__, __LINE__, "CHECK_DISCARDED",                  \
                #expression, _error.get()).stream()

#define CHECK_FAILED(expression)                                        \
  for (const Option<std::string> _error = _checkFailed(expression);     \
       _error.isSome();)                                                \
    _CheckFatal(__FILE__, __LINE__, "CHECK_FAILED",                     \
                #expression, _error.get()).stream()


// Private structs/functions used for CHECK_*.

template <typename T>
Option<std::string> _checkPending(const process::Future<T>& f)
{
  if (f.isReady()) {
    return Some("is READY");
  } else if (f.isDiscarded()) {
    return Some("is DISCARDED");
  } else if (f.isFailed()) {
    return Some("is FAILED: " + f.failure());
  }
  CHECK(f.isPending());
  return None();
}


template <typename T>
Option<std::string> _checkReady(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Some("is PENDING");
  } else if (f.isDiscarded()) {
    return Some("is DISCARDED");
  } else if (f.isFailed()) {
    return Some("is FAILED: " + f.failure());
  }
  CHECK(f.isReady());
  return None();
}


template <typename T>
Option<std::string> _checkDiscarded(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Some("is PENDING");
  } else if (f.isReady()) {
    return Some("is READY");
  } else if (f.isFailed()) {
    return Some("is FAILED: " + f.failure());
  }
  CHECK(f.isDiscarded());
  return None();
}


template <typename T>
Option<std::string> _checkFailed(const process::Future<T>& f)
{
  if (f.isPending()) {
    return Some("is PENDING");
  } else if (f.isReady()) {
    return Some("is READY");
  } else if (f.isDiscarded()) {
    return Some("is DISCARDED");
  }
  CHECK(f.isFailed());
  return None();
}

// TODO(dhamon): CHECK_NPENDING, CHECK_NREADY, etc.

#endif // __PROCESS_CHECK_HPP__
