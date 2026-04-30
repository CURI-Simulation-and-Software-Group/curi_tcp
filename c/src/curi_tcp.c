#include "curi_tcp.h"
#include <sys/time.h>   // defines struct timeval
#include <errno.h>

int tcp_init(tcp_node* p, char server_ip[], int server_port, int buffer_size, bool is_server){
#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;
#endif 
    // Safe Memory Allocation
    p->receive_buffer = (char*)malloc(buffer_size);
    p->send_buffer = (char*)malloc(buffer_size);
    if (!p->receive_buffer || !p->send_buffer) {
        if (p->receive_buffer) free(p->receive_buffer);
        if (p->send_buffer) free(p->send_buffer);
        p->receive_buffer = p->send_buffer = NULL;
        return -2; 
    }

    // Create Socket
    p->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p->server_fd < 0) return -5;

    // Address Configuration
    memset(&(p->server_addr), 0, sizeof(p->server_addr));
    p->server_addr.sin_family = AF_INET;
    p->server_addr.sin_port = htons(server_port);

    if (is_server) {
        // Use INADDR_ANY for server unless a specific IP is required
        p->server_addr.sin_addr.s_addr = (server_ip && strlen(server_ip) > 0) ? inet_addr(server_ip) : INADDR_ANY;
        
        if (bind(p->server_fd, (struct sockaddr*)&(p->server_addr), sizeof(p->server_addr)) == -1) {
            perror("bind error");
            #ifdef WIN32
                closesocket(p->server_fd);
            #else
                close(p->server_fd);
            #endif
            return -1;
        }
        
        p->client_fd = -1;
        listen(p->server_fd, 1);
    }else {
        p->server_addr.sin_addr.s_addr = inet_addr(server_ip);
        if (connect(p->server_fd, (struct sockaddr*)&p->server_addr, sizeof(p->server_addr)) < 0) {
            return -7;
        }
        // In client mode, server_fd is the active communication line
        p->client_fd = p->server_fd; 
        setsockopt(p->client_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_size, sizeof(int));
    }
    return 0;
}

int tcp_server_wait_client(tcp_node* p, int timeout_usec, int buffer_size){
    if (p->server_fd < 0 || p->client_fd != -1 || p->server_fd == p->client_fd) return -1;

    FD_ZERO(&(p->rset)); // Must clear and reset every time
    FD_SET(p->server_fd, &(p->rset));
    
    struct timeval* t_ptr = NULL;
    if (timeout_usec >= 0){
        struct timeval t;
        t.tv_sec = timeout_usec / 1000000;
        t.tv_usec = timeout_usec % 1000000;
        t_ptr = &t;
    }
    
    int nready = select(p->server_fd + 1, &(p->rset), NULL, NULL, t_ptr);
    
    if (nready > 0){
        socklen_t addr_len = sizeof(p->client_addr);
        p->client_fd = accept(p->server_fd, (struct sockaddr *)&p->client_addr, &addr_len); 
        if (p->client_fd < 0){
            return -2; // An actual error occurred
        }else{
            char current_byte;
            char last_byte;
            ssize_t bytes;
            while ((bytes = recv(p->client_fd, &current_byte, 1, MSG_DONTWAIT)) > 0); // Wait for the last response byte;

            if (bytes == 0) {
                // res == 0 means the client already sent a FIN (disconnected)
                tcp_server_clear_client(p);
                return tcp_server_wait_client(p, timeout_usec, buffer_size);
            }else if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                setsockopt(p->client_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_size, sizeof(int));
                return 0;
            }else{
                return -3; // An actual error occurred
            }
        }
    }else if (nready < 0){
        return -4; // An actual error occurred
    }
    return 1; // Timeout of select
}

void tcp_server_clear_client(tcp_node* p){
    if (p->client_fd != -1 && p->client_fd != p->server_fd) {
#ifdef WIN32
        closesocket(p->client_fd);
#else
        close(p->client_fd);
#endif
        p->client_fd = -1;
    }
}

int tcp_server_has_client(tcp_node* p){
    return (p->client_fd != -1 && p->client_fd != p->server_fd);
}

int tcp_server_get_client_info(tcp_node* p, char ip[], int port){
    if (!tcp_server_has_client(p)) return -1;
    strcpy(ip, inet_ntoa(p->client_addr.sin_addr));
    port = ntohs(p->client_addr.sin_port);
    return 0;
}

int tcp_select(tcp_node* p, int timeout_usec, int buffer_size){
    FD_ZERO(&(p->rset)); // Must clear and reset every time
    FD_SET(p->client_fd, &(p->rset));
    
    struct timeval t;
    t.tv_sec = timeout_usec / 1000000;
    t.tv_usec = timeout_usec % 1000000;

    int nready = select(p->client_fd + 1, &(p->rset), NULL, NULL, &t);
    
    if (nready > 0 && FD_ISSET(p->client_fd, &(p->rset))) {
        memset(p->receive_buffer, 0, buffer_size);
        p->receive_size = recv(p->client_fd, p->receive_buffer, buffer_size, 0);
        // If recv returns 0, the robot disconnected
        if (p->receive_size <= 0) return -1; 
    } else {
        p->receive_size = 0;
    }
    return p->receive_size;
}

void tcp_send(tcp_node* p, int buffer_size){
    if (p->client_fd < 0) return;
    
    // Use standard send for TCP. No need for server_addr here.
    int sent = send(p->client_fd, p->send_buffer, strlen(p->send_buffer), 0);
    
    if (sent < 0) {
        perror("TCP Send failed");
    }
}

int tcp_receive(tcp_node* p, int buffer_size){
    if (p->client_fd < 0) return -1;

    // Clear buffer before receiving
    memset(p->receive_buffer, 0, buffer_size);

    p->receive_size = recv(p->client_fd, p->receive_buffer, buffer_size - 1, 0);

    if (p->receive_size == 0)
        return 0;  // Robot gracefully closed the connection
    else if (p->receive_size < 0)
        return -1; // A real error occurred (or timeout if SO_RCVTIMEO was set)

    return p->receive_size;
}

void tcp_print(tcp_node* p, int buffer_size){
    if (p->receive_size > 0) {
        // Ensure we don't overflow the buffer when null-terminating
        int term_index = (p->receive_size >= buffer_size) ? buffer_size - 1 : p->receive_size;
        p->receive_buffer[term_index] = '\0';
        puts(p->receive_buffer);
    }
}

void tcp_close(tcp_node* p){
    if (p->receive_buffer) { free(p->receive_buffer); p->receive_buffer = NULL; }
    if (p->send_buffer) { free(p->send_buffer); p->send_buffer = NULL; }

#ifdef WIN32
    if (p->client_fd != -1) closesocket(p->client_fd);
    if (p->server_fd != -1) closesocket(p->server_fd);
    WSACleanup();
#else
    if (p->client_fd != -1) close(p->client_fd);
    if (p->server_fd != -1) close(p->server_fd);
#endif
}