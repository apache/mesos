#ifndef CONFIG_HPP
#define CONFIG_HPP

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

#endif /* CONFIG_HPP */
