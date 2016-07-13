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

#ifndef __PROCESS_GC_HPP__
#define __PROCESS_GC_HPP__

#include <map>

#include <process/process.hpp>


namespace process {

class GarbageCollector : public Process<GarbageCollector>
{
public:
  GarbageCollector() : ProcessBase("__gc__") {}
  virtual ~GarbageCollector() {}

  template <typename T>
  void manage(const T* t)
  {
    const ProcessBase* process = t;
    if (process != nullptr) {
      processes[process->self()] = process;
      link(process->self());
    }
  }

protected:
  virtual void exited(const UPID& pid)
  {
    if (processes.count(pid) > 0) {
      const ProcessBase* process = processes[pid];
      processes.erase(pid);
      delete process;
    }
  }

private:
  std::map<UPID, const ProcessBase*> processes;
};

} // namespace process {

#endif // __PROCESS_GC_HPP__
