// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_REFERENCE_HPP__
#define __PROCESS_REFERENCE_HPP__

#include <process/process.hpp>

namespace process {

// Provides reference counting semantics for a process pointer.
class ProcessReference
{
public:
  ProcessReference() : process(nullptr) {}

  ~ProcessReference()
  {
    cleanup();
  }

  ProcessReference(const ProcessReference& that)
  {
    copy(that);
  }

  ProcessReference& operator=(const ProcessReference& that)
  {
    if (this != &that) {
      cleanup();
      copy(that);
    }
    return *this;
  }

  ProcessBase* operator->() const
  {
    return process;
  }

  operator ProcessBase*() const
  {
    return process;
  }

  operator bool() const
  {
    return process != nullptr;
  }

private:
  friend class ProcessManager; // For ProcessManager::use.

  explicit ProcessReference(ProcessBase* _process)
    : process(_process)
  {
    if (process != nullptr) {
      process->refs.fetch_add(1);
    }
  }

  void copy(const ProcessReference& that)
  {
    process = that.process;

    if (process != nullptr) {
      // There should be at least one reference to the process, so
      // we don't need to worry about checking if it's exiting or
      // not, since we know we can always create another reference.
      CHECK(process->refs.load() > 0);
      process->refs.fetch_add(1);
    }
  }

  void cleanup()
  {
    if (process != nullptr) {
      process->refs.fetch_sub(1);
    }
  }

  ProcessBase* process;
};

} // namespace process {

#endif // __PROCESS_REFERENCE_HPP__
