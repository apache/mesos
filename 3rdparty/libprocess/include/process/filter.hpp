#ifndef __PROCESS_FILTER_HPP__
#define __PROCESS_FILTER_HPP__

#include <process/event.hpp>

namespace process {

class Filter {
public:
  virtual ~Filter() {}
  virtual bool filter(const MessageEvent& event) { return false; }
  virtual bool filter(const DispatchEvent& event) { return false; }
  virtual bool filter(const HttpEvent& event) { return false; }
  virtual bool filter(const ExitedEvent& event) { return false; }
};


// Use the specified filter on messages that get enqueued (note,
// however, that you cannot filter timeout messages).
void filter(Filter* filter);

} // namespace process {

#endif // __PROCESS_FILTER_HPP__
