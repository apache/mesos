#ifndef __PROCESS_IO_HPP__
#define __PROCESS_IO_HPP__

#include <cstring> // For size_t.
#include <string>

#include <process/future.hpp>

#include <stout/nothing.hpp>

namespace process {
namespace io {

// Possible events for polling.
const short READ = 0x01;
const short WRITE = 0x02;

// Buffered read chunk size. Roughly 16 pages.
const size_t BUFFERED_READ_SIZE = 16*4096;

// TODO(benh): Add a version which takes multiple file descriptors.
// Returns the events (a subset of the events specified) that can be
// performed on the specified file descriptor without blocking.
Future<short> poll(int fd, short events);


// Performs a single non-blocking read by polling on the specified
// file descriptor until any data can be be read. The future will
// become ready when some data is read (may be less than that
// specified by size). A failure will be returned if an error is
// detected. If end-of-file is reached, value zero will be
// returned. Note that the return type of this function differs from
// the standard 'read'. In particular, this function returns the
// number of bytes read or zero on end-of-file (an error is indicated
// by failing the future, thus only a 'size_t' is necessary rather
// than a 'ssize_t').
Future<size_t> read(int fd, void* data, size_t size);


// Performs a series of asynchronous reads, until EOF is reached.
// NOTE: When using this, ensure the sender will close the connection
// so that EOF can be reached.
//
// NOTE: the specified file descriptor is duplicated and set to
// close-on-exec and made non-blocking (which will return a failure if
// these operations can not be performed).
Future<std::string> read(int fd);


// Performs a non-blocking write by polling on the specified file
// descriptor until data can be be written. The future will become
// ready when some data is written with the number of bytes that were
// written. This may be less than the specified size of the data. A
// failure will be returned if an error is detected. In the special
// case of writing to a socket or pipe, an error will be returned if
// the read end of the socket or pipe has been closed.
Future<size_t> write(int fd, void* data, size_t size);


// Performs a series of asynchronous writes until all of data has been
// written or an error occured in which case a failure is returned.
//
// NOTE: the specified file descriptor is duplicated and set to
// close-on-exec and made non-blocking (which will return a failure if
// these operations can not be performed).
Future<Nothing> write(int fd, const std::string& data);


// Redirect output from 'from' file descriptor to 'to' file descriptor
// or /dev/null if 'to' is None. Note that depending on how we
// redirect output we duplicate the 'from' and 'to' file descriptors
// so we can control their lifetimes. Returns after EOF has been hit
// on 'from' or some form of failure has occured.
Future<Nothing> redirect(int from, Option<int> to, size_t chunk = 4096);

} // namespace io {
} // namespace process {

#endif // __PROCESS_IO_HPP__
