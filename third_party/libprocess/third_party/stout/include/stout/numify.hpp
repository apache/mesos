#ifndef __STOUT_NUMIFY_HPP__
#define __STOUT_NUMIFY_HPP__

#include <string>

#include <boost/lexical_cast.hpp>

#include "error.hpp"
#include "none.hpp"
#include "option.hpp"
#include "result.hpp"
#include "try.hpp"

template <typename T>
Try<T> numify(const std::string& s)
{
  try {
    return boost::lexical_cast<T>(s);
  } catch (const boost::bad_lexical_cast&) {
    return Error("Failed to convert '" + s + "' to number");
  }
}


template <typename T>
Result<T> numify(const Option<std::string>& s)
{
  if (s.isSome()) {
    Try<T> t = numify<T>(s.get());
    if (t.isSome()) {
      return t.get();
    } else if (t.isError()) {
      return Error(t.error());
    }
  }

  return None();
}

#endif // __STOUT_NUMIFY_HPP__
