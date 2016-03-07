// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_EXIT_HPP__
#define __STOUT_EXIT_HPP__

#include <stdlib.h>

#include <iostream> // For std::cerr.
#include <ostream>
#include <sstream>
#include <string>

#include <stout/attributes.hpp>


// Exit takes an exit status and provides a stream for output prior to
// exiting. This is like glog's LOG(FATAL) or CHECK, except that it
// does _not_ print a stack trace.
//
// Ex: EXIT(EXIT_FAILURE) << "Cgroups are not present in this system.";
#define EXIT(status) __Exit(status).stream()


struct __Exit
{
  __Exit(int _status) : status(_status) {}

  NORETURN ~__Exit()
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
