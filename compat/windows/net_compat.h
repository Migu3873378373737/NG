#ifndef NG_WIN_NET_COMPAT_H
#define NG_WIN_NET_COMPAT_H

/*
 * NG Windows Winsock compatibility layer (x64).
 * Inicializacao/finalizacao do Winsock e tipos/constantes basicicas de socket
 * equivalentes ao conjunto POSIX <sys/socket.h> + <netinet/in.h>.
 */
#ifndef _WIN32
#define _WIN32
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#define socket_t SOCKET
#define close_socket closesocket

static inline int ng_win_net_init(void) {
    WSADATA wsa;
    memset(&wsa, 0, sizeof(wsa));
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    return 0;
}

static inline void ng_win_net_cleanup(void) {
    WSACleanup();
}

/* htons/htonl/ntohs/ntohl via Winsock */
static inline unsigned short ng_win_htons(unsigned short v) { return htons(v); }
static inline unsigned int ng_win_htonl(unsigned int v) { return htonl(v); }
static inline unsigned short ng_win_ntohs(unsigned short v) { return ntohs(v); }
static inline unsigned int ng_win_ntohl(unsigned int v) { return ntohl(v); }

#endif /* NG_WIN_NET_COMPAT_H */
