// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MESOS_NATIVE_COMMON_HPP
#define MESOS_NATIVE_COMMON_HPP

// Python.h must be included before standard headers.
// See: http://docs.python.org/2/c-api/intro.html#include-files
#include <Python.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>


namespace mesos { namespace python {

/**
 * The Python module object for mesos_pb2 (which contains the protobuf
 * classes generated for Python).
 */
extern PyObject* mesos_pb2;


/**
 * RAII utility class for acquiring the Python global interpreter lock.
 */
class InterpreterLock {
  PyGILState_STATE state;

public:
  InterpreterLock() {
    state = PyGILState_Ensure();
  }

  ~InterpreterLock() {
    PyGILState_Release(state);
  }
};


/**
 * Convert a Python protocol buffer object into a C++ one by serializing
 * it to a string and deserializing the result back in C++. Returns true
 * on success, or prints an error and returns false on failure.
 */
template <typename T>
bool readPythonProtobuf(PyObject* obj, T* t)
{
  if (obj == Py_None) {
    std::cerr << "None object given where protobuf expected" << std::endl;
    return false;
  }
  PyObject* res = PyObject_CallMethod(obj,
                                      (char*) "SerializeToString",
                                      (char*) nullptr);
  if (res == nullptr) {
    std::cerr << "Failed to call Python object's SerializeToString "
         << "(perhaps it is not a protobuf?)" << std::endl;
    PyErr_Print();
    return false;
  }
  char* chars;
  Py_ssize_t len;
  if (PyString_AsStringAndSize(res, &chars, &len) < 0) {
    std::cerr << "SerializeToString did not return a string" << std::endl;
    PyErr_Print();
    Py_DECREF(res);
    return false;
  }
  google::protobuf::io::ArrayInputStream stream(chars, len);
  bool success = t->ParseFromZeroCopyStream(&stream);
  if (!success) {
    std::cerr << "Could not deserialize protobuf as expected type" << std::endl;
  }
  Py_DECREF(res);
  return success;
}


/**
 * Convert a C++ protocol buffer object into a Python one by serializing
 * it to a string and deserializing the result back in Python. Returns the
 * resulting PyObject* on success or raises a Python exception and returns
 * nullptr on failure.
 */
template <typename T>
PyObject* createPythonProtobuf(const T& t, const char* typeName)
{
  PyObject* dict = PyModule_GetDict(mesos_pb2);
  if (dict == nullptr) {
    PyErr_Format(PyExc_Exception, "PyModule_GetDict failed");
    return nullptr;
  }

  PyObject* type = PyDict_GetItemString(dict, typeName);
  if (type == nullptr) {
    PyErr_Format(PyExc_Exception, "Could not resolve mesos_pb2.%s", typeName);
    return nullptr;
  }
  if (!PyType_Check(type)) {
    PyErr_Format(PyExc_Exception, "mesos_pb2.%s is not a type", typeName);
    return nullptr;
  }

  std::string str;
  if (!t.SerializeToString(&str)) {
    PyErr_Format(PyExc_Exception, "C++ %s SerializeToString failed", typeName);
    return nullptr;
  }

  // Propagates any exception that might happen in FromString.
  return PyObject_CallMethod(type,
                             (char*) "FromString",
                             (char*) "s#",
                             str.data(),
                             str.size());
}


// construct<T>() should take PyObject* as an argument, try to convert that
// PyObject into an object of type T and return an object of type equivalent
// to std::unique_ptr<T> that should hold
//  - the conversion result on success
//  - nullptr on failure
// Also, the Python exception should be set on conversion failure.
//
// TODO(asekretenko): use std::optional or stout Try instead of
// std::unique_ptr when they become available in this code.
//
// Declaration of 'construct<T>()' for protobufs.
template <typename T>
typename std::enable_if<
  std::is_base_of<google::protobuf::Message, T>::value,
  std::unique_ptr<T>>::type
construct(PyObject* obj)
{
  std::unique_ptr<T> result(new T());
  if (!readPythonProtobuf(obj, result.get())) {
    PyErr_Format(
        PyExc_TypeError,
        "Failed to construct %s from a Python object",
        result->GetDescriptor()->full_name().c_str());

    return nullptr;
  }

  return result;
}


// Declaration of 'construct<T>()' for non-protobufs.
template <typename T>
typename std::enable_if<
  !std::is_base_of<google::protobuf::Message, T>::value,
  std::unique_ptr<T>>::type
construct(PyObject* obj);


// TODO(asekretenko): move this specialization into .cpp file. That file will
// likely have to be put into a library (there is no simple way to use one
// source file in two python extensions that can be built concurrently).
template <>
inline std::unique_ptr<std::string> construct<std::string>(PyObject* obj)
{
  char* chars;
  Py_ssize_t len;
  if (PyString_AsStringAndSize(obj, &chars, &len) < 0) {
    PyErr_Print();
    PyErr_Format(
        PyExc_TypeError,
        "Cannot construct std::string from a non-string object");

    return nullptr;
  }

  return std::unique_ptr<std::string>(new std::string(chars, len));
}


template <typename T>
std::unique_ptr<std::vector<T>> constructFromIterable(PyObject* iterable)
{
  PyObject* pyIterator = PyObject_GetIter(iterable);
  if (pyIterator == nullptr) {
    PyErr_Format(
      PyExc_TypeError,
      "Cannot construct std::vector from a non-iterable object");
    return nullptr;
  }

  std::unique_ptr<std::vector<T>> result(new std::vector<T>());

  PyObject* pyItem;

  while ((pyItem = PyIter_Next(pyIterator)) != nullptr) {
    std::unique_ptr<T> item = construct<T>(pyItem);
    if (!item) {
      // An exception has already been set by construct<>().
      Py_DECREF(pyItem);
      Py_DECREF(pyIterator);
      return nullptr;
    }

    result->emplace_back(std::move(*item));
    Py_DECREF(pyItem);
  }

  Py_DECREF(pyIterator);

  if (PyErr_Occurred() != nullptr) {
    return nullptr;
  }

  return result;
}



} // namespace python {
} // namespace mesos {

#endif /* MESOS_NATIVE_COMMON_HPP */
