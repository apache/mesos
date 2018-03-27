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

#ifndef __PROCESS_SHARED_HPP__
#define __PROCESS_SHARED_HPP__

#include <atomic>
#include <cstddef>
#include <memory>

#include <glog/logging.h>

#include <process/future.hpp>

namespace process {

// Forward declaration.
template <typename T>
class Owned;


// Represents a shared pointer and therefore enforces 'const' access.
template <typename T>
class Shared
{
public:
  Shared();
  explicit Shared(T* t);
  /*implicit*/ Shared(std::nullptr_t) : Shared(static_cast<T*>(nullptr)) {}

  bool operator==(const Shared<T>& that) const;
  bool operator<(const Shared<T>& that) const;

  // Enforces const access semantics.
  const T& operator*() const;
  const T* operator->() const;
  const T* get() const;

  bool unique() const;

  void reset();
  void reset(T* t);
  void swap(Shared<T>& that);

  // Transfers ownership of the pointer by waiting for exclusive
  // access (i.e., no other Shared instances). This shared pointer
  // will be reset after this function is invoked. If multiple shared
  // pointers pointing to the same object all want to be upgraded,
  // only one of them may succeed and the rest will get failures.
  Future<Owned<T>> own();

private:
  struct Data
  {
    explicit Data(T* _t);
    ~Data();

    T* t;
    std::atomic_bool owned;
    Promise<Owned<T>> promise;
  };

  std::shared_ptr<Data> data;
};


template <typename T>
Shared<T>::Shared() {}


template <typename T>
Shared<T>::Shared(T* t)
{
  if (t != nullptr) {
    data.reset(new Data(t));
  }
}


template <typename T>
bool Shared<T>::operator==(const Shared<T>& that) const
{
  return data == that.data;
}


template <typename T>
bool Shared<T>::operator<(const Shared<T>& that) const
{
  return data < that.data;
}


template <typename T>
const T& Shared<T>::operator*() const
{
  return *CHECK_NOTNULL(get());
}


template <typename T>
const T* Shared<T>::operator->() const
{
  return CHECK_NOTNULL(get());
}


template <typename T>
const T* Shared<T>::get() const
{
  if (data == nullptr) {
    return nullptr;
  } else {
    return data->t;
  }
}


template <typename T>
bool Shared<T>::unique() const
{
  return data.unique();
}


template <typename T>
void Shared<T>::reset()
{
  data.reset();
}


template <typename T>
void Shared<T>::reset(T* t)
{
  if (t == nullptr) {
    data.reset();
  } else {
    data.reset(new Data(t));
  }
}


template <typename T>
void Shared<T>::swap(Shared<T>& that)
{
  data.swap(that.data);
}


template <typename T>
Future<Owned<T>> Shared<T>::own()
{
  // If two threads simultaneously access this object and at least one
  // of them is a write, the behavior is undefined. This is similar to
  // boost::shared_ptr. For more details, please refer to the boost
  // shared_ptr document (section "Thread Safety").
  if (data == nullptr) {
    return Owned<T>(nullptr);
  }

  bool false_value = false;
  if (!data->owned.compare_exchange_strong(false_value, true)) {
    return Failure("Ownership has already been transferred");
  }

  Future<Owned<T>> future = data->promise.future();
  data.reset();
  return future;
}


template <typename T>
Shared<T>::Data::Data(T* _t)
  : t(CHECK_NOTNULL(_t)), owned(false) {}


template <typename T>
Shared<T>::Data::~Data()
{
  if (owned.load()) {
    promise.set(Owned<T>(t));
  } else {
    delete t;
  }
}

} // namespace process {

#endif // __PROCESS_SHARED_HPP__
