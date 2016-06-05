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

#ifndef __PROCESS_OWNED_HPP__
#define __PROCESS_OWNED_HPP__

#include <atomic>
#include <memory>

#include <glog/logging.h>

namespace process {

// Forward declaration.
template <typename T>
class Shared;


// Represents a uniquely owned pointer.
//
// TODO(bmahler): For now, Owned only provides shared_ptr semantics.
// When we make the switch to C++11, we will change to provide
// unique_ptr semantics. Consequently, each usage of Owned that
// invokes a copy will have to be adjusted to use move semantics.
template <typename T>
class Owned
{
public:
  Owned();
  explicit Owned(T* t);

  bool operator==(const Owned<T>& that) const;
  bool operator<(const Owned<T>& that) const;

  T& operator*() const;
  T* operator->() const;
  T* get() const;

  void reset();
  void reset(T* t);
  void swap(Owned<T>& that);

  // Converts from an owned pointer to a shared pointer. This owned
  // pointer will be reset after this function is invoked.
  Shared<T> share();

  // Converts from an owned pointer to a raw pointer. This owned
  // pointer will be reset after this function is invoked.
  T* release();

private:
  struct Data
  {
    explicit Data(T* t);
    ~Data();

    std::atomic<T*> t;
  };

  std::shared_ptr<Data> data;
};


template <typename T>
Owned<T>::Owned() {}


template <typename T>
Owned<T>::Owned(T* t)
{
  if (t != nullptr) {
    data.reset(new Data(t));
  }
}


template <typename T>
bool Owned<T>::operator==(const Owned<T>& that) const
{
  return data == that.data;
}


template <typename T>
bool Owned<T>::operator<(const Owned<T>& that) const
{
  return data < that.data;
}


template <typename T>
T& Owned<T>::operator*() const
{
  return *CHECK_NOTNULL(get());
}


template <typename T>
T* Owned<T>::operator->() const
{
  return CHECK_NOTNULL(get());
}


template <typename T>
T* Owned<T>::get() const
{
  if (data.get() == nullptr) {
    return nullptr;
  } else {
    // Static cast to avoid ambiguity in Visual Studio compiler.
    CHECK(data->t != static_cast<T*>(nullptr))
      << "This owned pointer has already been shared";

    return data->t;
  }
}


template <typename T>
void Owned<T>::reset()
{
  data.reset();
}


template <typename T>
void Owned<T>::reset(T* t)
{
  if (t == nullptr) {
    data.reset();
  } else {
    data.reset(new Data(t));
  }
}


template <typename T>
void Owned<T>::swap(Owned<T>& that)
{
  data.swap(that.data);
}


template <typename T>
Shared<T> Owned<T>::share()
{
  if (data.get() == nullptr) {
    // The ownership of this pointer has already been lost.
    return Shared<T>(nullptr);
  }

  // Atomically set the pointer 'data->t' to `nullptr`.
  T* old = data->t.exchange(nullptr);
  if (old == nullptr) {
    // The ownership of this pointer has already been lost.
    return Shared<T>(nullptr);
  }

  data.reset();
  return Shared<T>(old);
}


template <typename T>
T* Owned<T>::release()
{
  if (data.get() == nullptr) {
    // The ownership of this pointer has already been lost.
    return nullptr;
  }

  // Atomically set the pointer 'data->t' to `nullptr`.
  T* old = data->t.exchange(nullptr);
  if (old == nullptr) {
    // The ownership of this pointer has already been lost.
    return nullptr;
  }

  data.reset();
  return old;
}


template <typename T>
Owned<T>::Data::Data(T* _t)
  : t(CHECK_NOTNULL(_t)) {}


template <typename T>
Owned<T>::Data::~Data()
{
  delete t.load();
}

} // namespace process {

#endif // __PROCESS_OWNED_HPP__
