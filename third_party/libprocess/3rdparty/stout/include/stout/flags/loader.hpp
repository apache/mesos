#ifndef __STOUT_FLAGS_LOADER_HPP__
#define __STOUT_FLAGS_LOADER_HPP__

#include <string>

#include <tr1/functional>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;

template <typename T>
struct Loader
{
  static Try<Nothing> load(
      T* flag,
      const std::tr1::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = t.get();
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  }
};


template <typename T>
struct OptionLoader
{
  static Try<Nothing> load(
      Option<T>* flag,
      const std::tr1::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = Option<T>::some(t.get());
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  }
};


template <typename F, typename T>
struct MemberLoader
{
  static Try<Nothing> load(
      FlagsBase* base,
      T F::*flag,
      const std::tr1::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = t.get();
      } else {
        return Error("Failed to load value '" + value + "': " + t.error());
      }
    }
    return Nothing();
  }
};


template <typename F, typename T>
struct OptionMemberLoader
{
  static Try<Nothing> load(
      FlagsBase* base,
      Option<T> F::*flag,
      const std::tr1::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = Option<T>::some(t.get());
      } else {
        return Error("Failed to load value '" + value + "': " + t.error());
      }
    }
    return Nothing();
  }
};

} // namespace flags {

#endif // __STOUT_FLAGS_LOADER_HPP__
