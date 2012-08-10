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

#include <string>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/os.hpp>
#include <stout/time.hpp>

#include "logging/logging.hpp"

#include "slave/gc.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::string;

namespace mesos {
namespace internal {
namespace slave {

class GarbageCollectorProcess : public Process<GarbageCollectorProcess>
{
public:
  // GarbageCollector implementation.
  Future<bool> schedule(const seconds& s, const string& path);

private:
  void remove(const string& path, Promise<bool>* promise);
};


Future<bool> GarbageCollectorProcess::schedule(
    const seconds& s,
    const string& path)
{
  LOG(INFO) << "Scheduling " << path << " for removal";

  Promise<bool>* promise = new Promise<bool>();

  delay(s.value, self(), &Self::remove, path, promise);

  return promise->future();
}


void GarbageCollectorProcess::remove(
    const string& path,
    Promise<bool>* promise)
{
  LOG(INFO) << "Removing " << path;

  // TODO(benh): Check error conditions of 'rmdir', e.g., permission
  // denied, file no longer exists, etc.
  bool result = os::rmdir(path);

  promise->set(result);
  delete promise;
}


GarbageCollector::GarbageCollector()
{
  process = new GarbageCollectorProcess();
  spawn(process);
}


GarbageCollector::~GarbageCollector()
{
  terminate(process);
  wait(process);
}


Future<bool> GarbageCollector::schedule(
    const seconds& s,
    const string& path)
{
  return dispatch(process, &GarbageCollectorProcess::schedule, s, path);
}

} // namespace mesos {
} // namespace internal {
} // namespace slave {
