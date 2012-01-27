#ifndef __PROCESS_MESSAGE_HPP__
#define __PROCESS_MESSAGE_HPP__

#include <string>

#include <process/pid.hpp>

namespace process {

struct Message
{
  std::string name;
  UPID from;
  UPID to;
  std::string body;
};

} // namespace process {

#endif // __PROCESS_MESSAGE_HPP__
