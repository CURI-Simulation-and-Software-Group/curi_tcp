#ifndef CURI_TCP_H
#define CURI_TCP_H

#ifdef WIN32
	#include <Winsock2.h>
	#pragma comment(lib, "WS2_32.lib")
#else
	#include <arpa/inet.h> 
	#include <netinet/in.h> 
	#include <sys/socket.h> 
	#include <sys/types.h> 
	#include <stdint.h>
	#include <sys/select.h>
#endif
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <unistd.h>
#include <stdbool.h>

typedef struct _tcp_node
{
	char* receive_buffer;
	char* send_buffer;
	int receive_size;
	int send_size;
	int client_fd;
	int server_fd;
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	fd_set rset;
    uint8_t lock;
}tcp_node;

#ifdef __cplusplus
extern "C" 
{
#endif

int  tcp_init(tcp_node* p, char server_ip[], int server_port, int buffer_size, bool is_server);
int  tcp_select(tcp_node* p, int timeout_usec, int buffer_size);
void tcp_send(tcp_node* p, int buffer_size);
int  tcp_receive(tcp_node* p, int buffer_size);
void tcp_print(tcp_node* p, int buffer_size);
void tcp_close(tcp_node* p);
int  tcp_server_wait_client(tcp_node* p, int timeout_usec, int buffer_size);
void tcp_server_clear_client(tcp_node* p);
int  tcp_server_has_client(tcp_node* p);
int  tcp_server_get_client_info(tcp_node* p, char ip[], int port);

#ifdef __cplusplus
}
#endif
#endif