#ifndef __STOUT_NUMIFY_HPP__
#define __STOUT_NUMIFY_HPP__

#include <glog/logging.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "format.hpp"
#include "try.hpp"

template <typename T>
Try<T> numify(const std::string& s)
{
  try {
    return boost::lexical_cast<T>(s);
  } catch (const boost::bad_lexical_cast&) {
    const Try<std::string>& message =
      strings::format("Failed to convert '%s' to number", s);
    CHECK(message.isSome());
    return Try<T>::error(message.get());
  }
}

// TODO(bmahler): Add a numify that takes an Option<string> to simplify
// http request handling logic.

#endif // __STOUT_NUMIFY_HPP__
