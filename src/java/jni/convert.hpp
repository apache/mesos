#ifndef __CONVERT_HPP__
#define __CONVERT_HPP__

#include <jni.h>


template <typename T>
jobject convert(JNIEnv* env, const T& t);

#endif // __CONVERT_HPP__
