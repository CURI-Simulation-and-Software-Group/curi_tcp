#include "curi_tcp.h"

#if defined(_WIN32) || defined(WIN32)
	#include <windows.h>
	#include <ws2tcpip.h>
	#include <errno.h>
	#pragma comment(lib, "WS2_32.lib")
	#pragma comment(lib, "legacy_stdio_definitions.lib")
	#pragma comment(lib, "ucrt.lib")
	#define CURI_TCP_INVALID_FD INVALID_SOCKET
	#define CURI_TCP_FAILED(ret) ((ret) == SOCKET_ERROR)
	#define CURI_TCP_CLOSE_FD(fd) closesocket(fd)
static int curi_tcp_wsa_startup(void)
{
	WSADATA wsaData;
	return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

static void curi_tcp_wsa_cleanup(void)
{
	WSACleanup();
}

static int g_curi_tcp_wsa_refcount = 0;

static int curi_tcp_wsa_acquire(void)
{
	if (g_curi_tcp_wsa_refcount == 0) {
		if (curi_tcp_wsa_startup() != 0) {
			return -1;
		}
	}
	g_curi_tcp_wsa_refcount++;
	return 0;
}

static void curi_tcp_wsa_release(void)
{
	if (g_curi_tcp_wsa_refcount <= 0) {
		return;
	}
	g_curi_tcp_wsa_refcount--;
	if (g_curi_tcp_wsa_refcount == 0) {
		curi_tcp_wsa_cleanup();
	}
}
#else
	#include <arpa/inet.h>
	#include <netinet/tcp.h>
	#include <sys/types.h>
	#include <unistd.h>
	#include <sys/time.h>
	#include <errno.h>
	#include <fcntl.h>
	#define CURI_TCP_INVALID_FD (-1)
	#define CURI_TCP_FAILED(ret) ((ret) < 0)
	#define CURI_TCP_CLOSE_FD(fd) close(fd)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int curi_tcp_socket_errno(void)
{
#if defined(_WIN32) || defined(WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

static int curi_tcp_is_recv_would_block(int err)
{
#if defined(_WIN32) || defined(WIN32)
	return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
	return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

static int curi_tcp_is_in_progress(int err)
{
#if defined(_WIN32) || defined(WIN32)
	return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINTR;
#else
	return err == EINPROGRESS || err == EAGAIN || err == EINTR;
#endif
}

static int curi_tcp_set_nonblocking(int fd, int enable)
{
#if defined(_WIN32) || defined(WIN32)
	u_long mode = enable ? 1UL : 0UL;
	return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return -1;
	return fcntl(fd, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

static int64_t curi_tcp_now_usec(void)
{
#if defined(_WIN32) || defined(WIN32)
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER uli;
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	return (int64_t)(uli.QuadPart / 10LL - 11644473600LL * 1000000LL);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
#endif
}

static int curi_tcp_remaining_timeout_usec(int timeout_usec, int64_t deadline_usec)
{
	if (timeout_usec < 0) {
		return -1;
	}
	const int64_t now = curi_tcp_now_usec();
	const int64_t remaining = deadline_usec - now;
	if (remaining <= 0) {
		return 0;
	}
	if (remaining > 2147483647LL) {
		return 2147483647;
	}
	return (int)remaining;
}

static void curi_tcp_apply_socket_opts(int fd, int buffer_size)
{
	const int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_size, sizeof(int));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buffer_size, sizeof(int));
}

#if defined(_WIN32) || defined(WIN32)
static int curi_tcp_connect_with_timeout(SOCKET fd, const struct sockaddr* addr, int addrlen,
                                         int timeout_usec)
#else
static int curi_tcp_connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen,
                                         int timeout_usec)
#endif
{
	if (curi_tcp_set_nonblocking(fd, 1) != 0) {
		return -1;
	}

	const int connect_ret = connect(fd, addr, addrlen);
	if (connect_ret == 0) {
		curi_tcp_set_nonblocking(fd, 0);
		return 0;
	}

	const int connect_err = curi_tcp_socket_errno();
	if (!curi_tcp_is_in_progress(connect_err)) {
		return -1;
	}

	fd_set wset;
	FD_ZERO(&wset);
	FD_SET(fd, &wset);

	struct timeval tv;
	struct timeval* tv_ptr = NULL;
	if (timeout_usec >= 0) {
		tv.tv_sec = timeout_usec / 1000000;
		tv.tv_usec = timeout_usec % 1000000;
		tv_ptr = &tv;
	}

#if defined(_WIN32) || defined(WIN32)
	const int nready = select(0, NULL, &wset, NULL, tv_ptr);
#else
	const int nready = select(fd + 1, NULL, &wset, NULL, tv_ptr);
#endif
	if (nready <= 0) {
		return nready == 0 ? -2 : -1;
	}

	int so_error = 0;
#if defined(_WIN32) || defined(WIN32)
	int opt_len = (int)sizeof(so_error);
#else
	socklen_t opt_len = sizeof(so_error);
#endif
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &opt_len) != 0 || so_error != 0) {
		return -1;
	}

	curi_tcp_set_nonblocking(fd, 0);
	return 0;
}

int tcp_wait_readable(tcp_node* p, int timeout_usec)
{
	if (!p || CURI_TCP_IS_INVALID_FD(p->client_fd)) {
		return CURI_TCP_ERR_INVALID;
	}

	fd_set rset;
	FD_ZERO(&rset);
	FD_SET(p->client_fd, &rset);

	struct timeval tv;
	struct timeval* tv_ptr = NULL;
	if (timeout_usec >= 0) {
		if (timeout_usec == 0) {
			return 0;
		}
		tv.tv_sec = timeout_usec / 1000000;
		tv.tv_usec = timeout_usec % 1000000;
		tv_ptr = &tv;
	}

#if defined(_WIN32) || defined(WIN32)
	const int nready = select(0, &rset, NULL, NULL, tv_ptr);
#else
	const int nready = select(p->client_fd + 1, &rset, NULL, NULL, tv_ptr);
#endif
	if (nready > 0) {
		return 1;
	}
	if (nready == 0) {
		return 0;
	}
	return CURI_TCP_ERR_IO;
}

int tcp_write_all(tcp_node* p, const uint8_t* data, uint32_t len)
{
	if (!p || !data || len == 0 || CURI_TCP_IS_INVALID_FD(p->client_fd)) {
		return CURI_TCP_ERR_INVALID;
	}

	uint32_t sent = 0;
	while (sent < len) {
		const int chunk = (int)(len - sent);
#if defined(_WIN32) || defined(WIN32)
		const int n = send(p->client_fd, (const char*)(data + sent), chunk, 0);
#else
		const int n = (int)send(p->client_fd, data + sent, (size_t)chunk, 0);
#endif
		if (n == 0) {
			return CURI_TCP_ERR_DISCONNECT;
		}
		if (CURI_TCP_FAILED(n)) {
			return CURI_TCP_ERR_IO;
		}
		sent += (uint32_t)n;
	}
	return (int)sent;
}

int tcp_read_exact(tcp_node* p, uint8_t* out, uint32_t len, int timeout_usec)
{
	if (!p || !out || len == 0 || CURI_TCP_IS_INVALID_FD(p->client_fd)) {
		return CURI_TCP_ERR_INVALID;
	}
	if (timeout_usec == 0) {
		return 0;
	}

	const int64_t deadline_usec =
		timeout_usec < 0 ? 0 : curi_tcp_now_usec() + (int64_t)timeout_usec;

	uint32_t received = 0;
	while (received < len) {
		const int wait_timeout =
			curi_tcp_remaining_timeout_usec(timeout_usec, deadline_usec);
		if (wait_timeout == 0) {
			return 0;
		}

		const int wait = tcp_wait_readable(p, wait_timeout);
		if (wait <= 0) {
			return wait;
		}

		const int chunk = (int)(len - received);
#if defined(_WIN32) || defined(WIN32)
		const int n = recv(p->client_fd, (char*)(out + received), chunk, 0);
#else
		const int n = (int)recv(p->client_fd, out + received, (size_t)chunk, 0);
#endif
		if (n == 0) {
			return CURI_TCP_ERR_DISCONNECT;
		}
		if (CURI_TCP_FAILED(n)) {
			return CURI_TCP_ERR_IO;
		}
		received += (uint32_t)n;
	}
	return (int)received;
}

int tcp_init(tcp_node* p, const char* server_ip, int server_port, int buffer_size, bool is_server){
	return tcp_init_ex(p, server_ip, server_port, buffer_size, is_server,
	                   CURI_TCP_DEFAULT_CONNECT_TIMEOUT_USEC);
}

int tcp_init_ex(tcp_node* p, const char* server_ip, int server_port, int buffer_size, bool is_server,
                int client_connect_timeout_usec){
	int return_code = 0;
#if defined(_WIN32) || defined(WIN32)
	if (curi_tcp_wsa_acquire() != 0) {
		return -1;
	}
#endif

	p->receive_buffer = (char*)malloc(buffer_size);
	p->send_buffer = (char*)malloc(buffer_size);
	if (!p->receive_buffer || !p->send_buffer) {
		return_code = -2;
		goto cleanup_all;
	}

	p->server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (CURI_TCP_IS_INVALID_FD(p->server_fd)) {
		return_code = -3;
		goto cleanup_all;
	}

	memset(&(p->server_addr), 0, sizeof(p->server_addr));
	p->server_addr.sin_family = AF_INET;
	p->server_addr.sin_port = htons(server_port);

	if (is_server) {
		if ((server_ip && strlen(server_ip) > 0)){
			if (inet_pton(AF_INET, server_ip, &(p->server_addr.sin_addr)) <= 0) {
				fprintf(stderr, "inet_pton error: %d\n", curi_tcp_socket_errno());
				return_code = -4;
				goto cleanup_all;
			}
		}

		int reuse = 1;
		setsockopt(p->server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

		if (CURI_TCP_FAILED(bind(p->server_fd, (struct sockaddr*)&(p->server_addr), sizeof(p->server_addr)))) {
			fprintf(stderr, "bind error on ip %s and port %d: error %d\n",
				server_ip ? server_ip : "0.0.0.0", server_port, curi_tcp_socket_errno());
			return_code = -5;
			goto cleanup_all;
		}

		p->client_fd = CURI_TCP_INVALID_FD;
		if (CURI_TCP_FAILED(listen(p->server_fd, CURI_TCP_LISTEN_BACKLOG))) {
			return_code = -5;
			goto cleanup_all;
		}
	} else {
		if (!server_ip || inet_pton(AF_INET, server_ip, &(p->server_addr.sin_addr)) <= 0) {
			return_code = -6;
			goto cleanup_all;
		}

		const int connect_timeout = client_connect_timeout_usec < 0
			? CURI_TCP_DEFAULT_CONNECT_TIMEOUT_USEC
			: client_connect_timeout_usec;
		const int connect_ret = curi_tcp_connect_with_timeout(
			p->server_fd,
			(struct sockaddr*)&p->server_addr,
			sizeof(p->server_addr),
			connect_timeout);
		if (connect_ret != 0) {
			fprintf(stderr, "connect error on ip %s and port %d: error %d\n",
				server_ip ? server_ip : "0.0.0.0", server_port, curi_tcp_socket_errno());
			return_code = connect_ret == -2 ? -8 : -7;
			goto cleanup_all;
		}

		p->client_fd = p->server_fd;
		curi_tcp_apply_socket_opts(p->client_fd, buffer_size);
	}
	return 0;

cleanup_all:
	if (!CURI_TCP_IS_INVALID_FD(p->server_fd)) {
		CURI_TCP_CLOSE_FD(p->server_fd);
		p->server_fd = CURI_TCP_INVALID_FD;
	}
	if (p->receive_buffer) free(p->receive_buffer);
	if (p->send_buffer) free(p->send_buffer);
	p->receive_buffer = p->send_buffer = NULL;

#if defined(_WIN32) || defined(WIN32)
	if (return_code != 0) {
		curi_tcp_wsa_release();
	}
#endif

	return return_code;
}

int tcp_server_wait_client(tcp_node* p, int timeout_usec, int buffer_size){
	if (CURI_TCP_IS_INVALID_FD(p->server_fd) ||
		!CURI_TCP_IS_INVALID_FD(p->client_fd) ||
		p->server_fd == p->client_fd) {
		return -1;
	}

	FD_ZERO(&(p->rset));
	FD_SET(p->server_fd, &(p->rset));

	struct timeval t;
	struct timeval* t_ptr = NULL;
	if (timeout_usec >= 0){
		t.tv_sec = timeout_usec / 1000000;
		t.tv_usec = timeout_usec % 1000000;
		t_ptr = &t;
	}

#if defined(_WIN32) || defined(WIN32)
	int nready = select(0, &(p->rset), NULL, NULL, t_ptr);
#else
	int nready = select((int)p->server_fd + 1, &(p->rset), NULL, NULL, t_ptr);
#endif

	if (nready > 0){
#if defined(_WIN32) || defined(WIN32)
		int addr_len = (int)sizeof(p->client_addr);
#else
		socklen_t addr_len = sizeof(p->client_addr);
#endif
		p->client_fd = accept(p->server_fd, (struct sockaddr *)&p->client_addr, &addr_len);
		if (CURI_TCP_IS_INVALID_FD(p->client_fd)){
			return -2;
		}

		curi_tcp_apply_socket_opts(p->client_fd, buffer_size);
		return 0;
	} else if (nready < 0){
		return -4;
	}
	return 1;
}

void tcp_server_clear_client(tcp_node* p){
	if (!CURI_TCP_IS_INVALID_FD(p->client_fd) && p->client_fd != p->server_fd) {
		CURI_TCP_CLOSE_FD(p->client_fd);
		p->client_fd = CURI_TCP_INVALID_FD;
	}
}

int tcp_server_has_client(tcp_node* p){
	return !CURI_TCP_IS_INVALID_FD(p->client_fd) && p->client_fd != p->server_fd;
}

int tcp_server_get_client_info(tcp_node* p, char ip[], int* port_out){
	if (!tcp_server_has_client(p) || !ip || !port_out) return -1;
	inet_ntop(AF_INET, &(p->client_addr.sin_addr), ip, INET_ADDRSTRLEN);
	*port_out = (int)ntohs(p->client_addr.sin_port);
	return 0;
}

static int tcp_probe_client_disconnect(tcp_node* p)
{
	char ch;
	int probe_err = 0;
	int probe;

#if defined(_WIN32) || defined(WIN32)
	if (curi_tcp_set_nonblocking((int)p->client_fd, 1) != 0) {
		return 0;
	}
	probe = recv(p->client_fd, &ch, 1, MSG_PEEK);
	if (CURI_TCP_FAILED(probe)) probe_err = curi_tcp_socket_errno();
	curi_tcp_set_nonblocking((int)p->client_fd, 0);
#else
	probe = (int)recv(p->client_fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
	if (CURI_TCP_FAILED(probe)) probe_err = curi_tcp_socket_errno();
#endif

	if (probe == 0) {
		return 1;
	}
	if (CURI_TCP_FAILED(probe) && !curi_tcp_is_recv_would_block(probe_err)) {
		return 1;
	}
	return 0;
}

int tcp_select(tcp_node* p, int timeout_usec, int buffer_size){
	if (CURI_TCP_IS_INVALID_FD(p->client_fd)) {
		return CURI_TCP_ERR_INVALID;
	}

	FD_ZERO(&(p->rset));
	FD_SET(p->client_fd, &(p->rset));

	struct timeval t;
	struct timeval* t_ptr = NULL;
	if (timeout_usec >= 0) {
		t.tv_sec = timeout_usec / 1000000;
		t.tv_usec = timeout_usec % 1000000;
		t_ptr = &t;
	}

#if defined(_WIN32) || defined(WIN32)
	int nready = select(0, &(p->rset), NULL, NULL, t_ptr);
#else
	int nready = select((int)p->client_fd + 1, &(p->rset), NULL, NULL, t_ptr);
#endif

	p->receive_size = 0;
	if (nready > 0 && FD_ISSET(p->client_fd, &(p->rset))) {
		int bytes = recv(p->client_fd, p->receive_buffer, buffer_size, 0);
		if (bytes > 0) {
			p->receive_size = bytes;
			return bytes;
		}
		return CURI_TCP_ERR_DISCONNECT;
	} else if (nready < 0) {
		return CURI_TCP_ERR_IO;
	}

	if (tcp_probe_client_disconnect(p)) {
		return CURI_TCP_ERR_DISCONNECT;
	}
	return CURI_TCP_ERR_TIMEOUT;
}

int tcp_send_bytes(tcp_node* p, const void* data, int len)
{
	if (!p || !data || len <= 0 || CURI_TCP_IS_INVALID_FD(p->client_fd)) {
		return CURI_TCP_ERR_INVALID;
	}
	return tcp_write_all(p, (const uint8_t*)data, (uint32_t)len) == len ? 0 : CURI_TCP_ERR_IO;
}

void tcp_send(tcp_node* p, int buffer_size){
	if (CURI_TCP_IS_INVALID_FD(p->client_fd) || !p->send_buffer || buffer_size <= 0) {
		return;
	}

	const int len = (int)strnlen(p->send_buffer, (size_t)buffer_size);
	if (len <= 0) {
		return;
	}

	if (tcp_send_bytes(p, p->send_buffer, len) != 0) {
		perror("TCP Send failed");
	}
}

int tcp_receive(tcp_node* p, int buffer_size){
	if (CURI_TCP_IS_INVALID_FD(p->client_fd) || !p->receive_buffer || buffer_size <= 0) {
		return CURI_TCP_ERR_DISCONNECT;
	}

	p->receive_size = recv(p->client_fd, p->receive_buffer, buffer_size - 1, 0);

	if (p->receive_size == 0)
		return 0;
	else if (CURI_TCP_FAILED(p->receive_size))
		return CURI_TCP_ERR_DISCONNECT;

	if (p->receive_size < buffer_size) {
		p->receive_buffer[p->receive_size] = '\0';
	}
	return p->receive_size;
}

void tcp_print(tcp_node* p, int buffer_size){
	if (p->receive_size > 0) {
		int term_index = (p->receive_size >= buffer_size) ? buffer_size - 1 : p->receive_size;
		p->receive_buffer[term_index] = '\0';
		puts(p->receive_buffer);
	}
}

void tcp_close(tcp_node* p){
	if (p->receive_buffer) { free(p->receive_buffer); p->receive_buffer = NULL; }
	if (p->send_buffer) { free(p->send_buffer); p->send_buffer = NULL; }

	if (!CURI_TCP_IS_INVALID_FD(p->client_fd) && p->client_fd != p->server_fd) {
		CURI_TCP_CLOSE_FD(p->client_fd);
		p->client_fd = CURI_TCP_INVALID_FD;
	}
	if (!CURI_TCP_IS_INVALID_FD(p->server_fd)) {
		CURI_TCP_CLOSE_FD(p->server_fd);
		p->server_fd = CURI_TCP_INVALID_FD;
	}
	p->client_fd = CURI_TCP_INVALID_FD;

#if defined(_WIN32) || defined(WIN32)
	curi_tcp_wsa_release();
#endif
}
