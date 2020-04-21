// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_IO_HPP__
#define __PROCESS_IO_HPP__

#include <cstring> // For size_t.
#include <string>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

namespace process {
namespace io {

/**
 * A possible event while polling.
 *
 * @see process::io::poll
 */
const short READ = 0x01;

#ifndef ENABLE_LIBWINIO
/**
 * @copydoc process::io::READ
 */
const short WRITE = 0x02;
#endif // ENABLE_LIBWINIO

/**
 * Buffered read chunk size.
 *
 * Roughly 16 pages.
 */
const size_t BUFFERED_READ_SIZE = 16*4096;

/**
 * Prepares a file descriptor to be ready for asynchronous IO. On POSIX
 * systems, this sets the file descriptor to non-blocking. On Windows, this
 * will assign the file descriptor to an IO completion port.
 *
 * NOTE: Because the IO completion port is only known at the libprocess level,
 * we need this function instead of simply using stout's `os::nonblock` and
 * `os::isNonblock` functions like we could do for POSIX systems.
 *
 * @return On success, returns Nothing. On error, returns an Error.
 */
Try<Nothing> prepare_async(int_fd fd);


/**
 * Checks if `io::prepare_async` has been called on the file descriptor.
 *
 * @return Returns if the file descriptor is asynchronous. An asynchronous
 *     file descriptor is defined to be non-blocking on POSIX systems and
 *     overlapped and associated with an IO completion port on Windows.
 *     An error will be returned if the file descriptor is invalid.
 */
Try<bool> is_async(int_fd fd);


/**
 * Returns the events (a subset of the events specified) that can be
 * performed on the specified file descriptor without blocking.
 *
 * Note that on windows, only io::READ is available (under the
 * covers this is achieved via a zero byte read).
 *
 * @see process::io::READ
 * @see process::io::WRITE
 */
// TODO(benh): Add a version which takes multiple file descriptors.
Future<short> poll(int_fd fd, short events);


/**
 * Performs a single non-blocking read by polling on the specified
 * file descriptor until any data can be be read. `io::prepare_async`
 * needs to be called beforehand.
 *
 * The future will become ready when some data is read (may be less than
 * the specified size).
 *
 * To provide a consistent interface, a zero byte will immediately
 * return a ready future with 0 bytes. For users looking to use
 * the zero byte read trick on windows to achieve read readiness
 * polling, just use io::poll with io::READ.
 *
 * @return The number of bytes read or zero on EOF (or if zero
 *     bytes were requested).
 *     A failure will be returned if an error is detected.
 */
Future<size_t> read(int_fd fd, void* data, size_t size);


/**
 * Performs a series of asynchronous reads, until EOF is reached.
 *
 * **NOTE**: when using this, ensure the sender will close the connection
 * so that EOF can be reached.
 *
 * @return The concatentated result of the reads.
 *     A failure will be returned if the file descriptor is bad, or if the
 *     file descriptor cannot be duplicated, set to close-on-exec,
 *     or made non-blocking.
 */
Future<std::string> read(int_fd fd);


/**
 * Performs a single non-blocking write by polling on the specified
 * file descriptor until data can be be written. `io::prepare_async`
 * needs to be called beforehand.
 *
 * The future will become ready when some data is written (may be less than
 * the specified size of the data).
 *
 * @return The number of bytes written.
 *     A failure will be returned if an error is detected.
 *     If writing to a socket or pipe, an error will be returned if the
 *     the read end of the socket or pipe has been closed.
 */
Future<size_t> write(int_fd fd, const void* data, size_t size);


/**
 * Performs a series of asynchronous writes, until all of data has been
 * written.
 *
 * @return Nothing or a failure if an error occurred.
 *     A failure will be returned if the file descriptor is bad, or if the
 *     file descriptor cannot be duplicated, set to close-on-exec,
 *     or made non-blocking.
 */
Future<Nothing> write(int_fd fd, const std::string& data);

/**
 * Redirect output from the 'from' file descriptor to the 'to' file
 * descriptor (or /dev/null if 'to' is None). Optionally call a vector
 * of callback hooks, passing them the data before it is written to 'to'.
 *
 * The 'to' and 'from' file descriptors will be duplicated so that the
 * file descriptors' lifetimes can be controlled within this function.
 *
 * @return Nothing after EOF has been encountered on 'from' or if a
 *     failure has occurred. A failure will be returned if the file
 *     descriptor is bad, or if the file descriptor cannot be duplicated,
 *     set to close-on-exec, or made non-blocking.
 */
Future<Nothing> redirect(
    int_fd from,
    Option<int_fd> to,
    size_t chunk = 4096,
    const std::vector<lambda::function<void(const std::string&)>>& hooks = {});

} // namespace io {
} // namespace process {

#endif // __PROCESS_IO_HPP__
