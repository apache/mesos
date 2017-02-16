// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_GZIP_HPP__
#define __STOUT_GZIP_HPP__

#include <zlib.h>

#include <string>

#include <stout/abort.hpp>
#include <stout/error.hpp>
#include <stout/os/strerror.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


// Compression utilities.
// TODO(bmahler): Provide streaming compression as well.
namespace gzip {

namespace internal {

// We use a 16KB buffer with zlib compression / decompression.
#define GZIP_BUFFER_SIZE 16384


class GzipError : public Error
{
public:
  GzipError(const std::string& message, const z_stream_s& stream, int _code)
    : Error(message + ": " + GzipError::strerror(stream, _code)), code(_code) {}

  GzipError(const std::string& message, int _code)
    : Error(message + ": " + GzipError::strerror(_code)), code(_code) {}

  GzipError(const z_stream_s& stream, int _code)
    : Error(GzipError::strerror(stream, _code)), code(_code) {}

  GzipError(int _code)
    : Error(GzipError::strerror(_code)), code(_code) {}

  const int code;

private:
  static std::string strerror(int code)
  {
    // We do not attempt to interpret the error codes since
    // their meaning depends on which zlib call was made.
    switch (code) {
      case Z_OK:             return "Z_OK";           // Not an error.
      case Z_STREAM_END:     return "Z_STREAM_END";   // Not an error.
      case Z_NEED_DICT:      return "Z_NEED_DICT";    // Not an error.
      case Z_ERRNO:          return "Z_ERRNO: " + os::strerror(errno);
      case Z_STREAM_ERROR:   return "Z_STREAM_ERROR";
      case Z_DATA_ERROR:     return "Z_DATA_ERROR";
      case Z_MEM_ERROR:      return "Z_MEM_ERROR";
      case Z_BUF_ERROR:      return "Z_BUF_ERROR";
      case Z_VERSION_ERROR:  return "Z_VERSION_ERROR";
      default:               return "Unknown error " + stringify(code);
    }
  }

  static std::string strerror(const z_stream_s& stream, int code)
  {
    if (stream.msg == Z_NULL) {
      return GzipError::strerror(code);
    } else {
      return GzipError::strerror(code) + ": " + stream.msg;
    }
  }
};

} // namespace internal {


// Provides the ability to incrementally decompress
// a stream of compressed input data.
class Decompressor
{
public:
  Decompressor()
    : _finished(false)
  {
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.next_in = Z_NULL;
    stream.avail_in = 0;

    int code = inflateInit2(
        &stream,
        MAX_WBITS + 16); // Zlib magic for gzip compression / decompression.

    if (code != Z_OK) {
      Error error = internal::GzipError("Failed to inflateInit2", stream, code);
      ABORT(error.message);
    }
  }

  Decompressor(const Decompressor&) = delete;
  Decompressor& operator=(const Decompressor&) = delete;

  ~Decompressor()
  {
    if (inflateEnd(&stream) != Z_OK) {
      ABORT("Failed to inflateEnd");
    }
  }

  // Returns the next decompressed chunk of data,
  // or an Error if decompression fails.
  Try<std::string> decompress(const std::string& compressed)
  {
    stream.next_in =
      const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.length());

    // Build up the decompressed result.
    Bytef buffer[GZIP_BUFFER_SIZE];
    std::string result;

    while (stream.avail_in > 0) {
      stream.next_out = buffer;
      stream.avail_out = GZIP_BUFFER_SIZE;

      int code = inflate(&stream, Z_SYNC_FLUSH);

      _finished = code == Z_STREAM_END;

      if (code != Z_OK && !_finished) {
        return internal::GzipError("Failed to inflate", stream, code);
      }

      if (_finished && stream.avail_in > 0) {
        return Error("Stream finished with data unconsumed");
      }

      // Consume output and reset the buffer.
      result.append(
          reinterpret_cast<char*>(buffer),
          GZIP_BUFFER_SIZE - stream.avail_out);
      stream.next_out = buffer;
      stream.avail_out = GZIP_BUFFER_SIZE;
    }

    return result;
  }

  // Returns whether the decompression stream is finished.
  // If set to false, more input is expected.
  bool finished() const
  {
    return _finished;
  }

private:
  z_stream_s stream;
  bool _finished;
};


// Returns a gzip compressed version of the provided string.
// The compression level should be within the range [-1, 9].
// See zlib.h:
//   #define Z_NO_COMPRESSION         0
//   #define Z_BEST_SPEED             1
//   #define Z_BEST_COMPRESSION       9
//   #define Z_DEFAULT_COMPRESSION  (-1)
inline Try<std::string> compress(
    const std::string& decompressed,
    int level = Z_DEFAULT_COMPRESSION)
{
  // Verify the level is within range.
  if (!(level == Z_DEFAULT_COMPRESSION ||
      (level >= Z_NO_COMPRESSION && level <= Z_BEST_COMPRESSION))) {
    return Error("Invalid compression level: " + stringify(level));
  }

  z_stream_s stream;
  stream.next_in =
    const_cast<Bytef*>(reinterpret_cast<const Bytef*>(decompressed.data()));
  stream.avail_in = static_cast<uInt>(decompressed.length());
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;

  int code = deflateInit2(
      &stream,
      level,          // Compression level.
      Z_DEFLATED,     // Compression method.
      MAX_WBITS + 16, // Zlib magic for gzip compression / decompression.
      8,              // Default memLevel value.
      Z_DEFAULT_STRATEGY);

  if (code != Z_OK) {
    Error error = internal::GzipError("Failed to deflateInit2", stream, code);
    ABORT(error.message);
  }

  // Build up the compressed result.
  Bytef buffer[GZIP_BUFFER_SIZE];
  std::string result;
  do {
    stream.next_out = buffer;
    stream.avail_out = GZIP_BUFFER_SIZE;
    code = deflate(&stream, stream.avail_in > 0 ? Z_NO_FLUSH : Z_FINISH);

    if (code != Z_OK && code != Z_STREAM_END) {
      Error error = internal::GzipError("Failed to deflate", stream, code);
      if (deflateEnd(&stream) != Z_OK) {
        ABORT("Failed to deflateEnd");
      }
      return error;
    }

    // Consume output and reset the buffer.
    result.append(
        reinterpret_cast<char*>(buffer),
        GZIP_BUFFER_SIZE - stream.avail_out);
    stream.next_out = buffer;
    stream.avail_out = GZIP_BUFFER_SIZE;
  } while (code != Z_STREAM_END);

  if (deflateEnd(&stream) != Z_OK) {
    ABORT("Failed to deflateEnd");
  }

  return result;
}


// Returns a gzip decompressed version of the provided string.
inline Try<std::string> decompress(const std::string& compressed)
{
  Decompressor decompressor;
  Try<std::string> decompressed = decompressor.decompress(compressed);

  // Ensure that the decompression stream does not expect more input.
  if (decompressed.isSome() && !decompressor.finished()) {
    return Error("More input is expected");
  }

  return decompressed;
}

} // namespace gzip {

#endif // __STOUT_GZIP_HPP__
