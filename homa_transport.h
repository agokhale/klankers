#ifndef _HOMA_TRANSPORT_H_
#define _HOMA_TRANSPORT_H_

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <pthread.h>
#include "homa_protocol.h"
#include "homa_config.h"

#define HOMA_MAX_INFLIGHT_RPCS 256

struct homa_rx_rpc {
	uint64_t           rpc_id;
	struct sockaddr_in peer_addr;
	uint8_t            flags;
	uint32_t           total_length;
	uint32_t           received_bytes;
	uint32_t           granted_offset;
	char              *buffer;
	bool               complete;
	uint64_t           last_active_ms;
	bool               in_use;
};

struct homa_tx_rpc {
	uint64_t           rpc_id;
	struct sockaddr_in peer_addr;
	uint8_t            flags;
	uint32_t           total_length;
	uint32_t           sent_offset;
	uint32_t           granted_offset;
	const char        *buffer;
	bool               complete;
	uint64_t           last_active_ms;
	bool               in_use;
};

struct homa_transport {
	int                sockfd;
	uint16_t           port;
	uint32_t           unscheduled_bytes;
	uint32_t           max_grant_window;
	uint32_t           grant_increment;
	uint32_t           resend_interval_ms;

	pthread_mutex_t    lock;
	struct homa_rx_rpc rx_table[HOMA_MAX_INFLIGHT_RPCS];
	struct homa_tx_rpc tx_table[HOMA_MAX_INFLIGHT_RPCS];

	uint64_t           next_rpc_id;
};

int  homa_transport_init(struct homa_transport *t, const char *bind_addr,
         uint16_t port, const struct homa_config *cfg);
void homa_transport_close(struct homa_transport *t);

int  homa_transport_send_rpc(struct homa_transport *t, const struct sockaddr_in *dst,
         uint64_t rpc_id, uint8_t flags, const void *data, uint32_t len,
         int timeout_ms);

int  homa_transport_recv_rpc(struct homa_transport *t, struct sockaddr_in *src,
         uint64_t *out_rpc_id, uint8_t *out_flags, void **out_data,
         uint32_t *out_len, int timeout_ms);

uint64_t homa_next_rpc_id(struct homa_transport *t);

#endif /* _HOMA_TRANSPORT_H_ */
