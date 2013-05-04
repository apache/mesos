#ifndef __STOUT_EXIT_HPP__
#define __STOUT_EXIT_HPP__

#include <stdlib.h>

#include <iostream> // For std::cerr.
#include <ostream>
#include <sstream>
#include <string>

// Exit takes an exit status and provides a stream for output prior to
// exiting. This is like glog's LOG(FATAL) or CHECK, except that it
// does _not_ print a stack trace.
//
// Ex: EXIT(1) << "Cgroups are not present in this system.";
#define EXIT(status) __Exit(status).stream()

struct __Exit
{
  __Exit(int _status) : status(_status) {}

  ~__Exit()
  {
    std::cerr << out.str() << std::endl;
    exit(status);
  }

  std::ostream& stream()
  {
    return out;
  }

  std::ostringstream out;
  const int status;
};

#endif // __STOUT_EXIT_HPP__
