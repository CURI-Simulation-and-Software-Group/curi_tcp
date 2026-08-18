#ifndef CURI_TCP_FRAMED_H
#define CURI_TCP_FRAMED_H

#include "curi_tcp.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum payload size accepted by tcp_recv_frame (16 MiB). */
#define CURI_TCP_FRAME_MAX_PAYLOAD (16u * 1024u * 1024u)

/** Invalid frame length or payload larger than @p out_cap. */
#define CURI_TCP_FRAME_INVALID (-2)

/**
 * Send one length-prefixed binary frame: [uint32_be length][payload].
 * @return 0 on success, negative on error.
 */
int tcp_send_frame(tcp_node* p, const uint8_t* data, uint32_t len);

/**
 * Receive one length-prefixed frame into @p out (capacity @p out_cap).
 * @param timeout_usec Total budget for the full frame; <0 waits indefinitely.
 * @return payload length on success, 0 on timeout, negative on error/disconnect.
 */
int tcp_recv_frame(tcp_node* p, uint8_t* out, uint32_t out_cap, int timeout_usec);

#ifdef __cplusplus
}
#endif

#endif
