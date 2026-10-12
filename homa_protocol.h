#ifndef _HOMA_PROTOCOL_H_
#define _HOMA_PROTOCOL_H_

#include <stdint.h>
#include <sys/types.h>

#define HOMA_PORT_DEFAULT          9999
#define HOMA_MTU_DEFAULT           1500
#define HOMA_MAX_SEGMENT_SIZE      1400
#define HOMA_DEFAULT_UNSCHEDULED   16384
#define HOMA_DEFAULT_GRANT_WINDOW  65536
#define HOMA_DEFAULT_GRANT_INC     8192

#define HOMA_PACKET_DATA           0x10
#define HOMA_PACKET_GRANT          0x11
#define HOMA_PACKET_RESEND         0x12
#define HOMA_PACKET_BUSY           0x13
#define HOMA_PACKET_ACK            0x14

#define HOMA_FLAG_FROM_CLIENT      0x01
#define HOMA_FLAG_FROM_SERVER      0x02
#define HOMA_FLAG_RETRANSMIT       0x04

struct homa_common_hdr {
	uint16_t sport;
	uint16_t dport;
	uint8_t  type;
	uint8_t  flags;
	uint16_t checksum;
	uint64_t rpc_id;
} __attribute__((packed));

struct homa_data_hdr {
	struct homa_common_hdr common;
	uint32_t message_length;
	uint32_t offset;
	uint32_t unscheduled_limit;
	uint16_t seg_len;
	uint16_t flags;
} __attribute__((packed));

struct homa_grant_hdr {
	struct homa_common_hdr common;
	uint32_t offset;
	uint8_t  priority;
	uint8_t  resend_all;
	uint16_t reserved;
} __attribute__((packed));

struct homa_resend_hdr {
	struct homa_common_hdr common;
	uint32_t offset;
	uint32_t length;
	uint8_t  priority;
	uint8_t  reserved[3];
} __attribute__((packed));

struct homa_busy_hdr {
	struct homa_common_hdr common;
} __attribute__((packed));

struct homa_ack_hdr {
	struct homa_common_hdr common;
	uint16_t num_acks;
	uint16_t reserved;
} __attribute__((packed));

#endif /* _HOMA_PROTOCOL_H_ */
