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
  virtual bool filter(const MessageEvent&) { return false; }
  virtual bool filter(const DispatchEvent&) { return false; }
  virtual bool filter(const HttpEvent&) { return false; }
  virtual bool filter(const ExitedEvent&) { return false; }

  virtual bool filter(Event* event)
  {
    bool result = false;
    struct FilterVisitor : EventVisitor
    {
      explicit FilterVisitor(Filter* _filter, bool* _result)
        : filter(_filter), result(_result) {}

      virtual void visit(const MessageEvent& event)
      {
        *result = filter->filter(event);
      }

      virtual void visit(const DispatchEvent& event)
      {
        *result = filter->filter(event);
      }

      virtual void visit(const HttpEvent& event)
      {
        *result = filter->filter(event);
      }

      virtual void visit(const ExitedEvent& event)
      {
        *result = filter->filter(event);
      }

      Filter* filter;
      bool* result;
    } visitor(this, &result);

    event->visit(&visitor);

    return result;
  }
};


// Use the specified filter on messages that get enqueued (note,
// however, that you cannot filter timeout messages).
void filter(Filter* filter);

} // namespace process {

#endif // __PROCESS_FILTER_HPP__
