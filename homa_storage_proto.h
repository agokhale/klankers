#ifndef _HOMA_STORAGE_PROTO_H_
#define _HOMA_STORAGE_PROTO_H_

#include <stdint.h>

#define HOMA_BLK_MAGIC           0x484D5354
#define HOMA_BLK_VERSION         1

#define HOMA_BLK_OP_READ         0x01
#define HOMA_BLK_OP_WRITE        0x02
#define HOMA_BLK_OP_FLUSH        0x03
#define HOMA_BLK_OP_IDENTIFY     0x04

#define HOMA_BLK_FLAG_SYNC       0x01
#define HOMA_BLK_FLAG_DIRECT     0x02

#define HOMA_MAX_VOL_NAME_LEN    64

struct homa_blk_req {
	uint32_t magic;
	uint16_t version;
	uint16_t opcode;
	char     vol_name[HOMA_MAX_VOL_NAME_LEN];
	uint64_t offset;
	uint32_t length;
	uint32_t flags;
} __attribute__((packed));

struct homa_blk_resp {
	uint32_t magic;
	uint16_t version;
	int32_t  status;
	uint32_t length;
	uint64_t total_size;
	uint32_t block_size;
	uint32_t flags;
} __attribute__((packed));

#endif /* _HOMA_STORAGE_PROTO_H_ */
