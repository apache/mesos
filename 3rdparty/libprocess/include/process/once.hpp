#ifndef __PROCESS_ONCE_HPP__
#define __PROCESS_ONCE_HPP__

#include <process/future.hpp>

#include <stout/nothing.hpp>

namespace process {

// Provides a _blocking_ abstraction that's useful for performing a
// task exactly once.
class Once
{
public:
  Once() {}

  // Returns true if this Once instance has already transitioned to a
  // 'done' state (i.e., the action you wanted to perform "once" has
  // been completed). Note that this BLOCKS until Once::done has been
  // called.
  bool once()
  {
    if (!outer.set(&inner)) {
      inner.future().await();
      return true;
    }

    return false;
  }

  // Transitions this Once instance to a 'done' state.
  void done()
  {
    inner.set(Nothing());
  }

private:
  // Not copyable, not assignable.
  Once(const Once& that);
  Once& operator = (const Once& that);

  Promise<Nothing> inner;
  Promise<Promise<Nothing>*> outer;
};

}  // namespace process {

#endif // __PROCESS_ONCE_HPP__
