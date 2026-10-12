#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "homa_transport.h"

static uint64_t
get_monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

uint64_t
homa_next_rpc_id(struct homa_transport *t)
{
	uint64_t id;
	pthread_mutex_lock(&t->lock);
	id = ++t->next_rpc_id;
	pthread_mutex_unlock(&t->lock);
	return (id);
}

int
homa_transport_init(struct homa_transport *t, const char *bind_addr,
    uint16_t port, const struct homa_config *cfg)
{
	struct sockaddr_in sin;
	int val, rc;

	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->lock, NULL);

	t->port = port;
	t->unscheduled_bytes = cfg->unscheduled_bytes ? cfg->unscheduled_bytes : HOMA_DEFAULT_UNSCHEDULED;
	t->max_grant_window = cfg->max_grant_window ? cfg->max_grant_window : HOMA_DEFAULT_GRANT_WINDOW;
	t->grant_increment = cfg->grant_increment ? cfg->grant_increment : HOMA_DEFAULT_GRANT_INC;
	t->resend_interval_ms = cfg->resend_interval_ms ? cfg->resend_interval_ms : 100;
	t->next_rpc_id = 1000;

	t->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (t->sockfd < 0) {
		perror("socket(SOCK_DGRAM) failed");
		return (-1);
	}

	val = 1;
	setsockopt(t->sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	val = 4 * 1024 * 1024;
	setsockopt(t->sockfd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
	setsockopt(t->sockfd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (bind_addr == NULL || bind_addr[0] == '\0' || strcmp(bind_addr, "0.0.0.0") == 0) {
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		inet_pton(AF_INET, bind_addr, &sin.sin_addr);
	}

	rc = bind(t->sockfd, (struct sockaddr *)&sin, sizeof(sin));
	if (rc < 0) {
		perror("bind() failed");
		close(t->sockfd);
		return (-1);
	}

	return (0);
}

void
homa_transport_close(struct homa_transport *t)
{
	int i;

	if (t->sockfd >= 0) {
		close(t->sockfd);
		t->sockfd = -1;
	}

	pthread_mutex_lock(&t->lock);
	for (i = 0; i < HOMA_MAX_INFLIGHT_RPCS; i++) {
		if (t->rx_table[i].in_use && t->rx_table[i].buffer) {
			free(t->rx_table[i].buffer);
			t->rx_table[i].buffer = NULL;
		}
	}
	pthread_mutex_unlock(&t->lock);
	pthread_mutex_destroy(&t->lock);
}

static int
send_data_range(struct homa_transport *t, const struct sockaddr_in *dst,
    uint64_t rpc_id, uint8_t flags, uint32_t total_len, const char *buf,
    uint32_t from_offset, uint32_t to_offset)
{
	char packet[HOMA_MTU_DEFAULT];
	struct homa_data_hdr *dh = (struct homa_data_hdr *)packet;
	uint32_t cur = from_offset;

	while (cur < to_offset && cur < total_len) {
		uint32_t chunk = to_offset - cur;
		if (chunk > HOMA_MAX_SEGMENT_SIZE)
			chunk = HOMA_MAX_SEGMENT_SIZE;
		if (cur + chunk > total_len)
			chunk = total_len - cur;

		memset(dh, 0, sizeof(*dh));
		dh->common.sport = htons(t->port);
		dh->common.dport = dst->sin_port;
		dh->common.type = HOMA_PACKET_DATA;
		dh->common.flags = flags;
		dh->common.rpc_id = htobe64(rpc_id);

		dh->message_length = htonl(total_len);
		dh->offset = htonl(cur);
		dh->unscheduled_limit = htonl(t->unscheduled_bytes);
		dh->seg_len = htons((uint16_t)chunk);

		memcpy(packet + sizeof(*dh), buf + cur, chunk);

		sendto(t->sockfd, packet, sizeof(*dh) + chunk, 0,
		    (const struct sockaddr *)dst, sizeof(*dst));

		cur += chunk;
	}

	return (cur);
}

static void
send_grant(struct homa_transport *t, const struct sockaddr_in *dst,
    uint64_t rpc_id, uint32_t offset)
{
	struct homa_grant_hdr gh;

	memset(&gh, 0, sizeof(gh));
	gh.common.sport = htons(t->port);
	gh.common.dport = dst->sin_port;
	gh.common.type = HOMA_PACKET_GRANT;
	gh.common.rpc_id = htobe64(rpc_id);
	gh.offset = htonl(offset);
	gh.priority = 0;

	sendto(t->sockfd, &gh, sizeof(gh), 0,
	    (const struct sockaddr *)dst, sizeof(*dst));
}

int
homa_transport_send_rpc(struct homa_transport *t, const struct sockaddr_in *dst,
    uint64_t rpc_id, uint8_t flags, const void *data, uint32_t len,
    int timeout_ms)
{
	uint32_t unsched = (len < t->unscheduled_bytes) ? len : t->unscheduled_bytes;
	uint32_t sent_off;
	int tx_idx = -1, i;
	uint64_t start_ms = get_monotonic_ms();

	sent_off = send_data_range(t, dst, rpc_id, flags, len, (const char *)data, 0, unsched);
	if (sent_off >= len)
		return (0);

	pthread_mutex_lock(&t->lock);
	for (i = 0; i < HOMA_MAX_INFLIGHT_RPCS; i++) {
		if (!t->tx_table[i].in_use) {
			tx_idx = i;
			t->tx_table[i].in_use = true;
			t->tx_table[i].rpc_id = rpc_id;
			t->tx_table[i].peer_addr = *dst;
			t->tx_table[i].flags = flags;
			t->tx_table[i].total_length = len;
			t->tx_table[i].sent_offset = sent_off;
			t->tx_table[i].granted_offset = unsched;
			t->tx_table[i].buffer = (const char *)data;
			t->tx_table[i].complete = false;
			t->tx_table[i].last_active_ms = start_ms;
			break;
		}
	}
	pthread_mutex_unlock(&t->lock);

	if (tx_idx < 0) {
		fprintf(stderr, "Homa TX table exhausted!\n");
		return (-1);
	}

	while (1) {
		struct pollfd pfd;
		int pr;
		uint64_t now_ms = get_monotonic_ms();

		if (timeout_ms > 0 && (int)(now_ms - start_ms) > timeout_ms) {
			pthread_mutex_lock(&t->lock);
			t->tx_table[tx_idx].in_use = false;
			pthread_mutex_unlock(&t->lock);
			return (-ETIMEDOUT);
		}

		pthread_mutex_lock(&t->lock);
		if (t->tx_table[tx_idx].sent_offset >= t->tx_table[tx_idx].total_length) {
			t->tx_table[tx_idx].in_use = false;
			pthread_mutex_unlock(&t->lock);
			return (0);
		}
		pthread_mutex_unlock(&t->lock);

		pfd.fd = t->sockfd;
		pfd.events = POLLIN;
		pr = poll(&pfd, 1, 10);
		if (pr > 0 && (pfd.revents & POLLIN)) {
			char recv_buf[HOMA_MTU_DEFAULT];
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			ssize_t nr = recvfrom(t->sockfd, recv_buf, sizeof(recv_buf), 0,
			    (struct sockaddr *)&from, &from_len);
			if (nr >= (ssize_t)sizeof(struct homa_common_hdr)) {
				struct homa_common_hdr *ch = (struct homa_common_hdr *)recv_buf;
				uint64_t p_rpc_id = be64toh(ch->rpc_id);

				if (ch->type == HOMA_PACKET_GRANT && p_rpc_id == rpc_id) {
					struct homa_grant_hdr *gh = (struct homa_grant_hdr *)recv_buf;
					uint32_t granted = ntohl(gh->offset);

					pthread_mutex_lock(&t->lock);
					if (t->tx_table[tx_idx].in_use &&
					    t->tx_table[tx_idx].rpc_id == rpc_id) {
						uint32_t from_off = t->tx_table[tx_idx].sent_offset;
						uint32_t to_off = (granted > t->tx_table[tx_idx].total_length) ?
						    t->tx_table[tx_idx].total_length : granted;

						if (to_off > from_off) {
							uint32_t sent = send_data_range(t, dst, rpc_id, flags,
							    len, (const char *)data, from_off, to_off);
							t->tx_table[tx_idx].sent_offset = sent;
							t->tx_table[tx_idx].granted_offset = granted;
							t->tx_table[tx_idx].last_active_ms = get_monotonic_ms();
						}
					}
					pthread_mutex_unlock(&t->lock);
				}
			}
		}
	}
}

int
homa_transport_recv_rpc(struct homa_transport *t, struct sockaddr_in *src,
    uint64_t *out_rpc_id, uint8_t *out_flags, void **out_data,
    uint32_t *out_len, int timeout_ms)
{
	uint64_t start_ms = get_monotonic_ms();

	while (1) {
		struct pollfd pfd;
		int pr, i;
		uint64_t now_ms = get_monotonic_ms();

		pthread_mutex_lock(&t->lock);
		for (i = 0; i < HOMA_MAX_INFLIGHT_RPCS; i++) {
			if (t->rx_table[i].in_use && t->rx_table[i].complete) {
				*out_rpc_id = t->rx_table[i].rpc_id;
				*out_flags = t->rx_table[i].flags;
				*src = t->rx_table[i].peer_addr;
				*out_data = t->rx_table[i].buffer;
				*out_len = t->rx_table[i].total_length;

				t->rx_table[i].buffer = NULL;
				t->rx_table[i].in_use = false;
				pthread_mutex_unlock(&t->lock);
				return (0);
			}
		}
		pthread_mutex_unlock(&t->lock);

		if (timeout_ms > 0 && (int)(now_ms - start_ms) > timeout_ms)
			return (-ETIMEDOUT);

		pfd.fd = t->sockfd;
		pfd.events = POLLIN;
		pr = poll(&pfd, 1, 50);
		if (pr > 0 && (pfd.revents & POLLIN)) {
			char recv_buf[HOMA_MTU_DEFAULT];
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			ssize_t nr = recvfrom(t->sockfd, recv_buf, sizeof(recv_buf), 0,
			    (struct sockaddr *)&from, &from_len);

			if (nr >= (ssize_t)sizeof(struct homa_data_hdr)) {
				struct homa_data_hdr *dh = (struct homa_data_hdr *)recv_buf;
				if (dh->common.type == HOMA_PACKET_DATA) {
					uint64_t rpc_id = be64toh(dh->common.rpc_id);
					uint32_t total_len = ntohl(dh->message_length);
					uint32_t offset = ntohl(dh->offset);
					uint16_t seg_len = ntohs(dh->seg_len);
					int rx_idx = -1;

					if ((ssize_t)(sizeof(*dh) + seg_len) > nr)
						continue;

					pthread_mutex_lock(&t->lock);
					for (i = 0; i < HOMA_MAX_INFLIGHT_RPCS; i++) {
						if (t->rx_table[i].in_use &&
						    t->rx_table[i].rpc_id == rpc_id) {
							rx_idx = i;
							break;
						}
					}

					if (rx_idx < 0) {
						for (i = 0; i < HOMA_MAX_INFLIGHT_RPCS; i++) {
							if (!t->rx_table[i].in_use) {
								rx_idx = i;
								t->rx_table[i].in_use = true;
								t->rx_table[i].rpc_id = rpc_id;
								t->rx_table[i].peer_addr = from;
								t->rx_table[i].flags = dh->common.flags;
								t->rx_table[i].total_length = total_len;
								t->rx_table[i].received_bytes = 0;
								t->rx_table[i].granted_offset = t->unscheduled_bytes;
								t->rx_table[i].buffer = malloc(total_len > 0 ? total_len : 1);
								t->rx_table[i].complete = false;
								t->rx_table[i].last_active_ms = now_ms;
								break;
							}
						}
					}

					if (rx_idx >= 0 && t->rx_table[rx_idx].buffer != NULL) {
						struct homa_rx_rpc *rx = &t->rx_table[rx_idx];
						if (offset + seg_len <= rx->total_length) {
							memcpy(rx->buffer + offset, recv_buf + sizeof(*dh), seg_len);
							rx->received_bytes += seg_len;
							rx->last_active_ms = now_ms;

							if (rx->total_length > t->unscheduled_bytes &&
							    rx->received_bytes < rx->total_length) {
								if (rx->received_bytes + t->grant_increment >= rx->granted_offset) {
									uint32_t next_grant = rx->granted_offset + t->grant_increment;
									if (next_grant > rx->total_length)
										next_grant = rx->total_length;
									rx->granted_offset = next_grant;
									send_grant(t, &from, rpc_id, next_grant);
								}
							}

							if (rx->received_bytes >= rx->total_length)
								rx->complete = true;
						}
					}
					pthread_mutex_unlock(&t->lock);
				}
			}
		}
	}
}
