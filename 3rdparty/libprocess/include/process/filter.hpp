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

#ifndef __PROCESS_FILTER_HPP__
#define __PROCESS_FILTER_HPP__

#include <process/event.hpp>

namespace process {

class Filter {
public:
  virtual ~Filter() {}
  virtual bool filter(const UPID& process, const MessageEvent&)
  {
    return false;
  }
  virtual bool filter(const UPID& process, const DispatchEvent&)
  {
    return false;
  }
  virtual bool filter(const UPID& process, const HttpEvent&)
  {
    return false;
  }
  virtual bool filter(const UPID& process, const ExitedEvent&)
  {
    return false;
  }

  virtual bool filter(const UPID& process, Event* event)
  {
    bool result = false;
    struct FilterVisitor : EventVisitor
    {
      explicit FilterVisitor(
          Filter* _filter,
          const UPID& _process,
          bool* _result)
        : filter(_filter),
          process(_process),
          result(_result) {}

      void visit(const MessageEvent& event) override
      {
        *result = filter->filter(process, event);
      }

      void visit(const DispatchEvent& event) override
      {
        *result = filter->filter(process, event);
      }

      void visit(const HttpEvent& event) override
      {
        *result = filter->filter(process, event);
      }

      void visit(const ExitedEvent& event) override
      {
        *result = filter->filter(process, event);
      }

      Filter* filter;
      const UPID& process;
      bool* result;
    } visitor(this, process, &result);

    event->visit(&visitor);

    return result;
  }
};


// Use the specified filter on messages that get enqueued (note,
// however, that you cannot filter timeout messages).
void filter(Filter* filter);

} // namespace process {

#endif // __PROCESS_FILTER_HPP__
