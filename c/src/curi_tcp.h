#ifndef CURI_TCP_H
#define CURI_TCP_H

#if defined(_WIN32) || defined(WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <sys/select.h>
#endif

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(WIN32)
#define CURI_TCP_IS_INVALID_FD(fd) ((fd) == INVALID_SOCKET)
#else
#define CURI_TCP_IS_INVALID_FD(fd) ((fd) < 0)
#endif

/** Default connect timeout for client mode (10 seconds). */
#define CURI_TCP_DEFAULT_CONNECT_TIMEOUT_USEC (10 * 1000000)

/** listen() backlog for server mode. */
#define CURI_TCP_LISTEN_BACKLOG 8

/** Return codes shared by tcp_select / tcp_recv_frame (documented for callers). */
#define CURI_TCP_ERR_DISCONNECT (-1)
#define CURI_TCP_ERR_IO (-2)
#define CURI_TCP_ERR_INVALID (-3)
#define CURI_TCP_ERR_TIMEOUT (-4)

typedef struct _tcp_node
{
	char* receive_buffer;
	char* send_buffer;
	int receive_size;
	int send_size;

#if defined(_WIN32) || defined(WIN32)
	SOCKET client_fd;
	SOCKET server_fd;
#else
	int client_fd;
	int server_fd;
#endif

	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	fd_set rset;
	uint8_t lock;
} tcp_node;

#ifdef __cplusplus
extern "C"
{
#endif

int  tcp_init(tcp_node* p, const char* server_ip, int server_port, int buffer_size, bool is_server);
/** Like tcp_init; client_connect_timeout_usec < 0 uses CURI_TCP_DEFAULT_CONNECT_TIMEOUT_USEC. */
int  tcp_init_ex(tcp_node* p, const char* server_ip, int server_port, int buffer_size, bool is_server,
                int client_connect_timeout_usec);
int  tcp_select(tcp_node* p, int timeout_usec, int buffer_size);
void tcp_send(tcp_node* p, int buffer_size);
/** Send raw bytes (handles partial send). @return 0 on success, negative on error. */
int  tcp_send_bytes(tcp_node* p, const void* data, int len);
int  tcp_receive(tcp_node* p, int buffer_size);
void tcp_print(tcp_node* p, int buffer_size);
void tcp_close(tcp_node* p);
int  tcp_server_wait_client(tcp_node* p, int timeout_usec, int buffer_size);
void tcp_server_clear_client(tcp_node* p);
int  tcp_server_has_client(tcp_node* p);
int  tcp_server_get_client_info(tcp_node* p, char ip[], int* port_out);

/**
 * Write exactly @p len bytes on the connected client socket.
 * @return (int)len on success, CURI_TCP_ERR_DISCONNECT or CURI_TCP_ERR_IO on failure.
 */
int tcp_write_all(tcp_node* p, const uint8_t* data, uint32_t len);

/**
 * Read exactly @p len bytes into @p out.
 * @param timeout_usec Total budget; <0 waits indefinitely. 0 returns CURI_TCP_ERR_TIMEOUT immediately.
 * @return (int)len on success, 0 on timeout, negative on disconnect/error.
 */
int tcp_read_exact(tcp_node* p, uint8_t* out, uint32_t len, int timeout_usec);

/**
 * Wait until the client socket is readable.
 * @return 1 readable, 0 timeout, negative on error.
 */
int tcp_wait_readable(tcp_node* p, int timeout_usec);

#ifdef __cplusplus
}
#endif
#endif
