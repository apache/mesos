#ifndef __EVENT_LOOP_HPP__
#define __EVENT_LOOP_HPP__

#include <stout/duration.hpp>

namespace process {

// The interface that must be implemented by an event management
// system. This is a class to cleanly isolate the interface and so
// that in the future we can support multiple implementations.
class EventLoop
{
public:
  // Initializes the event loop.
  static void initialize();

  // Invoke the specified function in the event loop after the
  // specified duration.
  static void delay(const Duration& duration, void(*function)(void));

  // Returns the current time w.r.t. the event loop.
  static double time();

  // Runs the event loop.
  static void* run(void*);
};

} // namespace process {

#endif // __EVENT_LOOP_HPP__
