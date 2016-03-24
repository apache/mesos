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

#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __sun__ */

#ifdef __APPLE__
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __APPLE__ */

#ifdef __linux__
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __linux__ */

#ifdef __FreeBSD__
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#endif /* __FreeBSD__ */

#ifdef __WINDOWS__
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#endif /* __WINDOWS__ */

#endif // __CONFIG_HPP__
