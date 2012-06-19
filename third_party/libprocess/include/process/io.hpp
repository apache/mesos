#ifndef __PROCESS_IO_HPP__
#define __PROCESS_IO_HPP__

#include <process/future.hpp>

namespace process {
namespace io {

// Possible events for polling.
const short READ = 0x01;
const short WRITE = 0x02;

// Returns the events (a subset of the events specified) that can be
// performed on the specified file descriptor without blocking.
Future<short> poll(int fd, short events);

// TODO(benh): Add a version which takes multiple file descriptors.

} // namespace io {
} // namespace process {

#endif // __PROCESS_IO_HPP__
