// Winsock half of the host harness (kept apart: vmsock.h and winsock2.h clash).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

static SOCKET socks[8];

void hn_init(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }

// 1 = connected, -2 = DNS failure, -1 = connect failure
int hn_connect(const char *host, int port) {
	struct addrinfo hints, *res;
	char ps[8];
	SOCKET s;
	DWORD tmo = 20000;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	sprintf(ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &res)) return -2;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(s, res->ai_addr, (int)res->ai_addrlen)) { freeaddrinfo(res); return -1; }
	freeaddrinfo(res);
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
	socks[1] = s;
	return 1;
}
int hn_recv(int h, void *b, int n) { return recv(socks[h], b, n, 0); }
int hn_send(int h, const void *b, int n) { return send(socks[h], b, n, 0); }
void hn_close(int h) { closesocket(socks[h]); }
