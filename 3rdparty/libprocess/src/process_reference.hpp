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

class ProcessReference
{
public:
  ProcessReference() = default;

  ProcessReference(std::shared_ptr<ProcessBase*>&& reference)
    : reference(std::move(reference)) {}

  ProcessReference(const std::shared_ptr<ProcessBase*>& reference)
    : reference(reference) {}

  ProcessBase* operator->() const
  {
    CHECK(reference);
    return *reference;
  }

  operator ProcessBase*() const
  {
    CHECK(reference);
    return *reference;
  }

  operator bool() const
  {
    return (bool) reference;
  }

  std::shared_ptr<ProcessBase*> reference;
};

} // namespace process {

#endif // __PROCESS_REFERENCE_HPP__
