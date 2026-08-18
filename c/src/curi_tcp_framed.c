#include "curi_tcp_framed.h"

#include <string.h>

int tcp_send_frame(tcp_node* p, const uint8_t* data, uint32_t len)
{
    if (!p || !data || len == 0 || len > CURI_TCP_FRAME_MAX_PAYLOAD) {
        return CURI_TCP_ERR_INVALID;
    }
    if (CURI_TCP_IS_INVALID_FD(p->client_fd)) {
        return CURI_TCP_ERR_INVALID;
    }

    const uint8_t header[4] = {
        (uint8_t)((len >> 24) & 0xFFu),
        (uint8_t)((len >> 16) & 0xFFu),
        (uint8_t)((len >> 8) & 0xFFu),
        (uint8_t)(len & 0xFFu),
    };

    if (tcp_write_all(p, header, 4) != 4) {
        return CURI_TCP_ERR_IO;
    }
    if (tcp_write_all(p, data, len) != (int)len) {
        return CURI_TCP_ERR_IO;
    }
    return 0;
}

int tcp_recv_frame(tcp_node* p, uint8_t* out, uint32_t out_cap, int timeout_usec)
{
    if (!p || !out || out_cap == 0) {
        return CURI_TCP_ERR_INVALID;
    }
    if (CURI_TCP_IS_INVALID_FD(p->client_fd)) {
        return CURI_TCP_ERR_INVALID;
    }

    uint8_t header[4];
    const int header_read = tcp_read_exact(p, header, 4, timeout_usec);
    if (header_read <= 0) {
        return header_read;
    }

    const uint32_t len =
        ((uint32_t)header[0] << 24) |
        ((uint32_t)header[1] << 16) |
        ((uint32_t)header[2] << 8) |
         (uint32_t)header[3];

    if (len == 0 || len > CURI_TCP_FRAME_MAX_PAYLOAD || len > out_cap) {
        return CURI_TCP_FRAME_INVALID;
    }

    const int payload_read = tcp_read_exact(p, out, len, timeout_usec);
    if (payload_read != (int)len) {
        return payload_read <= 0 ? payload_read : CURI_TCP_ERR_IO;
    }
    return (int)len;
}
