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

#ifndef __IO_INTERNAL_HPP__
#define __IO_INTERNAL_HPP__

#include <process/future.hpp>

#include <stout/os/int_fd.hpp>

namespace process {
namespace io {
namespace internal {

#ifndef ENABLE_LIBWINIO
Future<size_t> read(int_fd fd, void* data, size_t size);
#else
Future<size_t> read(
    int_fd fd,
    void* data,
    size_t size,
    bool bypassZeroByteRead);
#endif // ENABLE_LIBWINIO

Future<size_t> write(int_fd fd, const void* data, size_t size);

Try<Nothing> prepare_async(int_fd fd);

Try<bool> is_async(int_fd fd);

} // namespace internal {
} // namespace io {
} // namespace process {

#endif // __IO_INTERNAL_HPP__
