#ifndef __PROCESS_DISPATCH_HPP__
#define __PROCESS_DISPATCH_HPP__

#include <process/process.hpp>

// Provides "dispatch" abiltiies for a process. That is, given a local
// (i.e., typed) pid, one can dispatch a method to it more naturally
// then sending a string and some bytes of data. This provides better
// type safety.

namespace process {

/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on receiver
 */
template <typename T>
void dispatch(const PID<T>& pid,
              void (T::*method)());

template <typename T>
void dispatch(const Process<T>& process,
              void (T::*method)());

template <typename T>
void dispatch(const Process<T>* process,
              void (T::*method)());


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 argument to pass to method
 */
template <typename T, typename P1, typename A1>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1),
              A1 a1);

template <typename T, typename P1, typename A1>
void dispatch(const Process<T>& process,
              void (T::*method)(P1),
              A1 a1);

template <typename T, typename P1, typename A1>
void dispatch(const Process<T>* process,
              void (T::*method)(P1),
              A1 a1);


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 */
template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2);

template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2);

template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2);


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 second argument to pass to method
 */
template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3);

template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3);

template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3);


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 */
template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4);

template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4);

template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4);


/**
 * Dispatches a void method on a process.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @param a5 fifth argument to pass to method
 */
template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)());

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)());

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)());


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1),
                   A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1),
                   A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1),
                   A1 a1);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 second argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @param a5 fifth argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on receiver
 */
template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)());

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)());

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)());


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on receiver
 * @param a1 first argument to pass to method
 */
template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1),
                   A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1),
                   A1 a1);

template <typename R, typename T, typename P1, typename A1>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1),
                   A1 a1);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 second argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4);


/**
 * Dispatches a method on a process and returns the future that
 * corresponds to the result of executing the method.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @param a5 fifth argument to pass to method
 * @return future corresponding to the result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @return result of executing the method
 */
template <typename R, typename T>
R call(const PID<T>& pid,
       Promise<R> (T::*method)());

template <typename R, typename T>
R call(const Process<T>& process,
       Promise<R> (T::*method)());

template <typename R, typename T>
R call(const Process<T>* process,
       Promise<R> (T::*method)());

/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename A1>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1),
       A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1),
       A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1),
       A1 a1);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2),
       A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2),
       A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2),
       A1 a1, A2 a2);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 second argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @param a5 fifth argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @return result of executing the method
 */
template <typename R, typename T>
R call(const PID<T>& pid,
       R (T::*method)());

template <typename R, typename T>
R call(const Process<T>& process,
       R (T::*method)());

template <typename R, typename T>
R call(const Process<T>* process,
       R (T::*method)());


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename A1>
R call(const PID<T>& pid,
       R (T::*method)(P1),
       A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>& process,
       R (T::*method)(P1),
       A1 a1);

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>* process,
       R (T::*method)(P1),
       A1 a1);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>& process,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2);

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>* process,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 second argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4);


/**
 * Dispatches a method on a process and waits (on the underlying
 * future) for the result.
 *
 * @param pid receiver of message
 * @param method method to invoke on instance
 * @param a1 first argument to pass to method
 * @param a2 second argument to pass to method
 * @param a3 third argument to pass to method
 * @param a4 fourth argument to pass to method
 * @param a5 fifth argument to pass to method
 * @return result of executing the method
 */
template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


namespace internal {

template <typename T>
void __associate(const Future<T>& future, Promise<T> promise)
{
  if (future.ready()) {
    promise.set(future.get());
  } else if (future.discarded()) {
    promise.future().discard();
  }
}


template <typename T>
void associate(const Promise<T>& from, const Promise<T>& to)
{
  std::tr1::function<void(const Future<T>&)> callback =
    std::tr1::bind(__associate<T>, std::tr1::placeholders::_1, to);
  Future<T> future = from.future();
  future.onReady(callback);
  future.onDiscarded(callback);
}


template <typename T>
void vdispatcher(ProcessBase* process,
                 std::tr1::function<void(T*)> thunk)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  thunk(t);
}


template <typename R, typename T>
void pdispatcher(ProcessBase* process,
                 std::tr1::function<Promise<R>(T*)> thunk,
                 Promise<R> promise)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  associate(thunk(t), promise);
}


template <typename R, typename T>
void dispatcher(ProcessBase* process,
                std::tr1::function<R(T*)> thunk,
                Promise<R> promise)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  promise.set(thunk(t));
}


typedef std::tr1::function<void(ProcessBase*)> Dispatcher;

// Dispatches a call on the specified process.
void dispatch(const UPID& pid, Dispatcher* dispatcher);


// Dispatches a call returning void on the specified process.
template <typename T>
void vdispatch(const UPID& pid,
               const std::tr1::function<void(T*)>& thunk)
{
  Dispatcher* dispatcher = new Dispatcher(
      std::tr1::bind(&internal::vdispatcher<T>,
                     std::tr1::placeholders::_1,
                     thunk));

  dispatch(pid, dispatcher);
}


// Dispatches a call returning a Promise on the specified process.
template <typename R, typename T>
Future<R> pdispatch(const UPID& pid,
                    const std::tr1::function<Promise<R>(T*)>& thunk)
{
  Promise<R> promise;

  Dispatcher* dispatcher = new Dispatcher(
      std::tr1::bind(&internal::pdispatcher<R, T>,
                     std::tr1::placeholders::_1,
                     thunk, promise));

  dispatch(pid, dispatcher);

  return promise.future();
}


// Dispatches a call for the specified process.
template <typename R, typename T>
Future<R> dispatch(const UPID& pid,
                   const std::tr1::function<R(T*)>& thunk)
{
  Promise<R> promise;

  Dispatcher* dispatcher = new Dispatcher(
      std::tr1::bind(&internal::dispatcher<R, T>,
                     std::tr1::placeholders::_1,
                     thunk, promise));

  dispatch(pid, dispatcher);

  return promise.future();
}

} // namespace internal {


/////////////////////////////////
// Returning void with 0 args. //
/////////////////////////////////

template <typename T>
void dispatch(const PID<T>& pid,
              void (T::*method)())
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1);

  internal::vdispatch(pid, thunk);
}


template <typename T>
void dispatch(const Process<T>& process,
              void (T::*method)())
{
  dispatch(process.self(), method);
}


template <typename T>
void dispatch(const Process<T>* process,
              void (T::*method)())
{
  dispatch(process->self(), method);
}


/////////////////////////////////
// Returning void with 1 args. //
/////////////////////////////////

template <typename T, typename P1, typename A1>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1),
              A1 a1)
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1);

  internal::vdispatch(pid, thunk);
}


template <typename T, typename P1, typename A1>
void dispatch(const Process<T>& process,
              void (T::*method)(P1),
              A1 a1)
{
  dispatch(process.self(), method, a1);
}


template <typename T, typename P1, typename A1>
void dispatch(const Process<T>* process,
              void (T::*method)(P1),
              A1 a1)
{
  dispatch(process->self(), method, a1);
}


/////////////////////////////////
// Returning void with 2 args. //
/////////////////////////////////

template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2)
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2);

  internal::vdispatch(pid, thunk);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2)
{
  dispatch(process.self(), method, a1, a2);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2),
              A1 a1, A2 a2)
{
  dispatch(process->self(), method, a1, a2);
}


/////////////////////////////////
// Returning void with 3 args. //
/////////////////////////////////

template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3);

  internal::vdispatch(pid, thunk);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3)
{
  dispatch(process.self(), method, a1, a2, a3);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3),
              A1 a1, A2 a2, A3 a3)
{
  dispatch(process->self(), method, a1, a2, a3);
}


/////////////////////////////////
// Returning void with 4 args. //
/////////////////////////////////

template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4);

  internal::vdispatch(pid, thunk);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4)
{
  dispatch(process.self(), method, a1, a2, a3, a4);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3, P4),
              A1 a1, A2 a2, A3 a3, A4 a4)
{
  dispatch(process->self(), method, a1, a2, a3, a4);
}


/////////////////////////////////
// Returning void with 5 args. //
/////////////////////////////////

template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const PID<T>& pid,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<void(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4, a5);

  internal::vdispatch(pid, thunk);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const Process<T>& process,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  dispatch(process.self(), method, a1, a2, a3, a4, a5);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
void dispatch(const Process<T>* process,
              void (T::*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  dispatch(process->self(), method, a1, a2, a3, a4, a5);
}


///////////////////////////////////////
// Returning Promise<R> with 0 args. //
///////////////////////////////////////

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)())
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1);

  return internal::pdispatch(pid, thunk);
}


template <typename R, typename T>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)())
{
  return dispatch(process.self(), method);
}


template <typename R, typename T>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)())
{
  return dispatch(process->self(), method);
}


///////////////////////////////////////
// Returning Promise<R> with 1 args. //
///////////////////////////////////////

template <typename R, typename T, typename P1, typename A1>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1),
                   A1 a1)
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1);

  return internal::pdispatch(pid, thunk);
}


template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1),
                   A1 a1)
{
  return dispatch(process.self(), method, a1);
}


template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1),
                   A1 a1)
{
  return dispatch(process->self(), method, a1);
}


///////////////////////////////////////
// Returning Promise<R> with 2 args. //
///////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2);

  return internal::pdispatch(pid, thunk);
}


template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  return dispatch(process.self(), method, a1, a2);
}


template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  return dispatch(process->self(), method, a1, a2);
}


///////////////////////////////////////
// Returning Promise<R> with 3 args. //
///////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3);

  return internal::pdispatch(pid, thunk);
}


template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  return dispatch(process.self(), method, a1, a2, a3);
}


template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  return dispatch(process->self(), method, a1, a2, a3);
}


///////////////////////////////////////
// Returning Promise<R> with 4 args. //
///////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4);

  return internal::pdispatch(pid, thunk);
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(process.self(), method, a1, a2, a3, a4);
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(process->self(), method, a1, a2, a3, a4);
}


///////////////////////////////////////
// Returning Promise<R> with 5 args. //
///////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const PID<T>& pid,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<Promise<R>(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4, a5);

  return internal::pdispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>& process,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(process.self(), method, a1, a2, a3, a4, a5);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>* process,
                   Promise<R> (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(process->self(), method, a1, a2, a3, a4, a5);
}


//////////////////////////////
// Returning R with 0 args. //
//////////////////////////////

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)())
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)())
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)())
{
  return dispatch(process->self(), method);
}


//////////////////////////////
// Returning R with 1 args. //
//////////////////////////////

template <typename R, typename T, typename P1, typename A1>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1),
                   A1 a1)
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename A1>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1),
                   A1 a1)
{
  return dispatch(process.self(), method, a1);
}

template <typename R, typename T, typename P1, typename A1>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1),
                   A1 a1)
{
  return dispatch(process->self(), method, a1);
}


//////////////////////////////
// Returning R with 2 args. //
//////////////////////////////

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  return dispatch(process.self(), method, a1, a2);
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2),
                   A1 a1, A2 a2)
{
  return dispatch(process->self(), method, a1, a2);
}


//////////////////////////////
// Returning R with 3 args. //
//////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  return dispatch(process.self(), method, a1, a2, a3);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3),
                   A1 a1, A2 a2, A3 a3)
{
  return dispatch(process->self(), method, a1, a2, a3);
}


//////////////////////////////
// Returning R with 4 args. //
//////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(process.self(), method, a2, a2, a3, a4);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3, P4),
                   A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(process->self(), method, a2, a2, a3, a4);
}


//////////////////////////////
// Returning R with 5 args. //
//////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const PID<T>& pid,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<R(T*)> thunk =
    std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4, a5);

  return internal::dispatch(pid, thunk);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>& process,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(process.self(), method, a1, a2, a3, a4, a5);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> dispatch(const Process<T>* process,
                   R (T::*method)(P1, P2, P3, P4, P5),
                   A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(process->self(), method, a1, a2, a3, a4, a5);
}


/////////////////////////////////////////////
// Returning R via Promise<R> with 0 args. //
/////////////////////////////////////////////

template <typename R, typename T>
R call(const PID<T>& pid,
       Promise<R> (T::*method)())
{
  return dispatch(pid, method).get();
}

template <typename R, typename T>
R call(const Process<T>& process,
       Promise<R> (T::*method)())
{
  return call(process.self(), method);
}


template <typename R, typename T>
R call(const Process<T>* process,
       Promise<R> (T::*method)())
{
  return call(process->self(), method);
}


/////////////////////////////////////////////
// Returning R via Promise<R> with 1 args. //
/////////////////////////////////////////////

template <typename R, typename T, typename P1, typename A1>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1), A1 a1)
{
  return dispatch(pid, method, a1).get();
}

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1),
       A1 a1)
{
  return call(process.self(), method, a1);
}

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1),
       A1 a1)
{
  return call(process->self(), method, a1);
}


/////////////////////////////////////////////
// Returning R via Promise<R> with 2 args. //
/////////////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2), A1 a1, A2 a2)
{
  return dispatch(pid, method, a1, a2).get();
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2),
       A1 a1, A2 a2)
{
  return call(process.self(), method, a1, a2);
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2),
       A1 a1, A2 a2)
{
  return call(process->self(), method, a1, a2);
}



/////////////////////////////////////////////
// Returning R via Promise<R> with 3 args. //
/////////////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return dispatch(pid, method, a1, a2, a3).get();
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return call(process.self(), method, a1, a2, a3);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return call(process->self(), method, a1, a2, a3);
}


/////////////////////////////////////////////
// Returning R via Promise<R> with 4 args. //
/////////////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(pid, method, a1, a2, a3, a4).get();
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return call(process.self(), method, a1, a2, a3, a4);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return call(process->self(), method, a1, a2, a3, a4);
}


/////////////////////////////////////////////
// Returning R via Promise<R> with 5 args. //
/////////////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const PID<T>& pid,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(pid, method, a1, a2, a3, a4, a5).get();
}


template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>& process,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return call(process.self(), method, a1, a2, a3, a4, a5);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>* process,
       Promise<R> (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return call(process->self(), method, a1, a2, a3, a4, a5);
}


////////////////////////////////////
// Returning R via R with 0 args. //
////////////////////////////////////

template <typename R, typename T>
R call(const PID<T>& pid,
       R (T::*method)())
{
  return dispatch(pid, method).get();
}

template <typename R, typename T>
R call(const Process<T>& process,
       R (T::*method)())
{
  return call(process.self(), method);
}

template <typename R, typename T>
R call(const Process<T>* process,
       R (T::*method)())
{
  return call(process->self(), method);
}


////////////////////////////////////
// Returning R via R with 1 args. //
////////////////////////////////////

template <typename R, typename T,
          typename P1, typename A1>
R call(const PID<T>& pid,
       R (T::*method)(P1),
       A1 a1)
{
  return dispatch(pid, method, a1).get();
}

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>& process,
       R (T::*method)(P1),
       A1 a1)
{
  return call(process.self(), method, a1);
}

template <typename R, typename T,
          typename P1, typename A1>
R call(const Process<T>* process,
       R (T::*method)(P1),
       A1 a1)
{
  return call(process->self(), method, a1);
}


////////////////////////////////////
// Returning R via R with 2 args. //
////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2)
{
  return dispatch(pid, method, a1, a2).get();
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>& process,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2)
{
  return call(process.self(), method, a1, a2);
}

template <typename R, typename T,
          typename P1, typename P2,
          typename A1, typename A2>
R call(const Process<T>* process,
       R (T::*method)(P1, P2),
       A1 a1, A2 a2)
{
  return call(process->self(), method, a1, a2);
}


////////////////////////////////////
// Returning R via R with 3 args. //
////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return dispatch(pid, method, a1, a2, a3).get();
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return call (process.self(), method, a1, a2, a3);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3),
       A1 a1, A2 a2, A3 a3)
{
  return call (process->self(), method, a1, a2, a3);
}


////////////////////////////////////
// Returning R via R with 4 args. //
////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return dispatch(pid, method, a1, a2, a3, a4).get();
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return call(process.self(), method, a1, a2, a3, a4);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3, P4),
       A1 a1, A2 a2, A3 a3, A4 a4)
{
  return call(process->self(), method, a1, a2, a3, a4);
}


////////////////////////////////////
// Returning R via R with 5 args. //
////////////////////////////////////

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const PID<T>& pid,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return dispatch(pid, method, a1, a2, a3, a4, a5).get();
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>& process,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return call(process.self(), method, a1, a2, a3, a4, a5);
}

template <typename R, typename T,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
R call(const Process<T>* process,
       R (T::*method)(P1, P2, P3, P4, P5),
       A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  return call(process->self(), method, a1, a2, a3, a4, a5);
}

} // namespace process {

#endif // __PROCESS_DISPATCH_HPP__
