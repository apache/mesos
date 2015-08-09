/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#ifndef __PROCESS_OWNED_HPP__
#define __PROCESS_OWNED_HPP__

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
// invoked a copy will have to be adjusted to use move semantics.
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

    T* volatile t; // The pointer 't' is volatile.
  };

  std::shared_ptr<Data> data;
};


template <typename T>
Owned<T>::Owned() {}


template <typename T>
Owned<T>::Owned(T* t)
{
  if (t != NULL) {
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
  if (data.get() == NULL) {
    return NULL;
  } else {
    CHECK(data->t != NULL) << "This owned pointer has already been shared";

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
  if (t == NULL) {
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
  if (data.get() == NULL) {
    // The ownership of this pointer has already been lost.
    return Shared<T>(NULL);
  }

  // Atomically set the pointer 'data->t' to NULL.
  T* t = __sync_fetch_and_and(&data->t, NULL);
  if (t == NULL) {
    // The ownership of this pointer has already been lost.
    return Shared<T>(NULL);
  }

  data.reset();
  return Shared<T>(t);
}


template <typename T>
T* Owned<T>::release()
{
  if (data.get() == NULL) {
    // The ownership of this pointer has already been lost.
    return NULL;
  }

  // Atomically set the pointer 'data->t' to NULL.
  T* t = __sync_fetch_and_and(&data->t, NULL);
  if (t == NULL) {
    // The ownership of this pointer has already been lost.
    return NULL;
  }

  data.reset();
  return t;
}


template <typename T>
Owned<T>::Data::Data(T* _t)
  : t(CHECK_NOTNULL(_t)) {}


template <typename T>
Owned<T>::Data::~Data()
{
  if (t != NULL) {
    delete t;
  }
}

} // namespace process {

#endif // __PROCESS_OWNED_HPP__
