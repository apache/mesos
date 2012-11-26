#ifndef __STOUT_EXIT_HPP__
#define __STOUT_EXIT_HPP__

#include <stdlib.h>

#include <ostream>
#include <sstream>
#include <string>

// Exit takes an exit code and provides a stream for output prior to exiting.
// This is like LOG(FATAL) or CHECK, except that it does _not_ print a stack
// trace because we don't want to freak out the user.
//
// Ex: EXIT(1) << "Cgroups are not present in this system.";
#define EXIT __Exit().stream

struct __Exit
{
  ~__Exit()
  {
    std::cerr << out.str() << std::endl;
    exit(exitCode);
  }

  std::ostream& stream(int exitCode)
  {
    this->exitCode = exitCode;
    return out;
  }

  std::ostringstream out;
  int exitCode;
};

#endif // __STOUT_EXIT_HPP__
