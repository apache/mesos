#ifndef __PROCESS_CLOCK_HPP__
#define __PROCESS_CLOCK_HPP__

namespace process {

// Forward declarations.
class ProcessBase;

// TODO(bmahler): Update this to be Duration aware.
class Clock
{
public:
  static double now();
  static double now(ProcessBase* process);
  static void pause();
  static bool paused();
  static void resume();
  static void advance(double secs);
  static void advance(ProcessBase* process, double secs);
  static void update(double secs);
  static void update(ProcessBase* process, double secs);
  static void order(ProcessBase* from, ProcessBase* to);
  static void settle();
};

} // namespace process {

#endif // __PROCESS_CLOCK_HPP__
