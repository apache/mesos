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

#endif // __CONFIG_HPP__
