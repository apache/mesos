#include <boost/unordered_map.hpp>

#include "hash_pid.hpp"

// TODO(benh): This should really be in libprocess.
std::size_t hash_value(const PID& pid)
{
  std::size_t seed = 0;
  boost::hash_combine(seed, pid.pipe);
  boost::hash_combine(seed, pid.ip);
  boost::hash_combine(seed, pid.port);
  return seed;
}
