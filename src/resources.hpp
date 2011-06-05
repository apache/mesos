#ifndef __RESOURCES_HPP__
#define __RESOURCES_HPP__

namespace mesos { namespace internal {

// Some memory unit constants.
const int64_t Kilobyte = 1024;
const int64_t Megabyte = 1024 * Kilobyte;
const int64_t Gigabyte = 1024 * Megabyte;


// A resource vector.
struct Resources {
  int32_t cpus;
  int64_t mem;

  Resources(): cpus(0), mem(0) {}
  
  Resources(int32_t _cpus, int64_t _mem): cpus(_cpus), mem(_mem) {}
  
  Resources operator + (const Resources& r) const
  {
    return Resources(cpus + r.cpus, mem + r.mem);
  }
  
  Resources operator - (const Resources& r) const
  {
    return Resources(cpus - r.cpus, mem - r.mem);
  }
  
  Resources& operator += (const Resources& r)
  {
    cpus += r.cpus;
    mem += r.mem;
    return *this;
  }

  Resources& operator -= (const Resources& r)
  {
    cpus -= r.cpus;
    mem -= r.mem;
    return *this;
  }
};


inline std::ostream& operator << (std::ostream& stream, const Resources& res)
{
  stream << "<" << res.cpus << " CPUs, " << res.mem << " MEM>";
  return stream;
}

}} /* namespace */

#endif /* __RESOURCES_HPP__ */
