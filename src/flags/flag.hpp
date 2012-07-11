#ifndef __FLAGS_FLAG_HPP__
#define __FLAGS_FLAG_HPP__

#include <string>

#include <tr1/functional>

namespace flags {

// Forward declaration.
class FlagsBase;

struct Flag
{
  std::string name;
  std::string help;
  bool boolean;
  std::tr1::function<void(FlagsBase*, const std::string&)> loader;
};

} // namespace flags {

#endif // __FLAGS_FLAG_HPP__
