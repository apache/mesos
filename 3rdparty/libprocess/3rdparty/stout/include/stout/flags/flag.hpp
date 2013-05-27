#ifndef __STOUT_FLAGS_FLAG_HPP__
#define __STOUT_FLAGS_FLAG_HPP__

#include <string>

#include <tr1/functional>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;

struct Flag
{
  std::string name;
  std::string help;
  bool boolean;
  std::tr1::function<Try<Nothing>(FlagsBase*, const std::string&)> loader;
};

} // namespace flags {

#endif // __STOUT_FLAGS_FLAG_HPP__
