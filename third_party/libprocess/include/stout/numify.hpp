#ifndef __STOUT_NUMIFY_HPP__
#define __STOUT_NUMIFY_HPP__

#include <string>

#include <boost/lexical_cast.hpp>

#include "format.hpp"
#include "option.hpp"
#include "result.hpp"
#include "try.hpp"

template <typename T>
Try<T> numify(const std::string& s)
{
  try {
    return boost::lexical_cast<T>(s);
  } catch (const boost::bad_lexical_cast&) {
    const Try<std::string>& message =
      strings::format("Failed to convert '%s' to number", s);

    // TODO(bmahler): Kill this when strings::format no longer returns a Try.
    if (!message.isSome()) {
      std::cerr << "Failed to format string " << message.error() << std::endl;
      abort();
    }

    return Try<T>::error(message.get());
  }
}


template <typename T>
Result<T> numify(const Option<std::string>& s)
{
  if (s.isSome()) {
    Try<T> t = numify<T>(s.get());
    if (t.isSome()) {
      return Result<T>::some(t.get());
    } else if (t.isError()) {
      return Result<T>::error(t.error());
    }
  }

  return Result<T>::none();
}

#endif // __STOUT_NUMIFY_HPP__
