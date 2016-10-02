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

/**
 * @copydoc process::io::READ
 */
const short WRITE = 0x02;

/**
 * Buffered read chunk size.
 *
 * Roughly 16 pages.
 */
const size_t BUFFERED_READ_SIZE = 16*4096;

/**
 * Returns the events (a subset of the events specified) that can be
 * performed on the specified file descriptor without blocking.
 *
 * @see process::io::READ
 * @see process::io::WRITE
 */
// TODO(benh): Add a version which takes multiple file descriptors.
Future<short> poll(int fd, short events);


/**
 * Performs a single non-blocking read by polling on the specified
 * file descriptor until any data can be be read.
 *
 * The future will become ready when some data is read (may be less than
 * the specified size).
 *
 * @return The number of bytes read or zero on EOF.
 *     A failure will be returned if an error is detected.
 */
Future<size_t> read(int fd, void* data, size_t size);


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
Future<std::string> read(int fd);
#ifdef __WINDOWS__
// Version of this function compatible with Windows `HANDLE`.
Future<std::string> read(HANDLE fd);
#endif // __WINDOWS__


/**
 * Performs a single non-blocking write by polling on the specified
 * file descriptor until data can be be written.
 *
 * The future will become ready when some data is written (may be less than
 * the specified size of the data).
 *
 * @return The number of bytes written.
 *     A failure will be returned if an error is detected.
 *     If writing to a socket or pipe, an error will be returned if the
 *     the read end of the socket or pipe has been closed.
 */
Future<size_t> write(int fd, const void* data, size_t size);


/**
 * Performs a series of asynchronous writes, until all of data has been
 * written.
 *
 * @return Nothing or a failure if an error occurred.
 *     A failure will be returned if the file descriptor is bad, or if the
 *     file descriptor cannot be duplicated, set to close-on-exec,
 *     or made non-blocking.
 */
Future<Nothing> write(int fd, const std::string& data);

/**
 * Redirect output from the 'from' file descriptor to the 'to' file
 * descriptor (or /dev/null if 'to' is None).
 *
 * The 'to' and 'from' file descriptors will be duplicated so that the
 * file descriptors' lifetimes can be controlled within this function.
 *
 * @return Nothing after EOF has been encountered on 'from' or if a
 *     failure has occurred. A failure will be returned if the file
 *     descriptor is bad, or if the file descriptor cannot be duplicated,
 *     set to close-on-exec, or made non-blocking.
 */
Future<Nothing> redirect(int from, Option<int> to, size_t chunk = 4096);
#ifdef __WINDOWS__
// Version of this function compatible with Windows `HANDLE`.
Future<Nothing> redirect(HANDLE from, Option<int> to, size_t chunk = 4096);
#endif // __WINDOWS__


/**
 * Performs a single non-blocking peek by polling on the specified
 * file descriptor until any data can be be peeked.
 *
 * The future will become ready when some data is peeked (may be less
 * than specified by the limit). A failure will be returned if an error
 * is detected. If end-of-file is reached, value zero will be returned.
 *
 * **NOTE**: This function is inspired by the MSG_PEEK flag of recv()
 * in that it does not remove the peeked data from the queue. Thus, a
 * subsequent io::read or io::peek() call will return the same data.
 *
 * TODO(hartem): This function will currently return an error if fd
 * is not a socket descriptor. Chnages need to be made to support
 * ordinary files and pipes as well.
 *
 * @param fd socket descriptor.
 * @param data buffer to which peek'd bytes will be copied.
 * @param size size of the buffer.
 * @param limit maximum number of bytes to peek.
 * @return The number of bytes peeked.
 *     A failure will be returned if an error is detected.
 */
Future<size_t> peek(int fd, void* data, size_t size, size_t limit);


/**
 * A more convenient version of io::peek that does not require
 * allocating the buffer.
 *
 * **NOTE**: this function treats the limit parameter merely as an
 * upper bound for the size of the data to peek. It does not wait
 * until the specified amount of bytes is peeked. It returns as soon
 * as some amount of data becomes available.
 * It cannot concatenate data from subsequent peeks because MSG_PEEK
 * has known limitations when it comes to spanning message boundaries.
 *
 * **NOTE**: this function will return an error if the limit is
 * greater than the internal peek buffer size (64k as of writing this
 * comment, io::BUFFERED_READ_SIZE. The caller should use the overlaod
 * of io::peek that allows to supply a bigger buffer.
 * TODO(hartem): It will be possible to fix this once SO_PEEK_OFF
 * (introduced in 3.4 kernels) becomes universally available.
 *
 * @param fd socket descriptor.
 * @param limit maximum number of bytes to peek.
 * @return Peeked bytes.
 *     A failure will be returned if an error is detected.
 */
Future<std::string> peek(int fd, size_t limit);

} // namespace io {
} // namespace process {

#endif // __PROCESS_IO_HPP__
