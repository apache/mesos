#ifndef __CONSTRUCT_HPP__
#define __CONSTRUCT_HPP__

#include <jni.h>


template <typename T>
T construct(JNIEnv *env, jobject jobj);

#endif /* __CONSTRUCT_HPP__ */
