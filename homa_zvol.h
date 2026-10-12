#ifndef _HOMA_ZVOL_H_
#define _HOMA_ZVOL_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "homa_config.h"

struct homa_zvol {
	char            name[64];
	char            path[HOMA_PATH_MAX];
	int             fd;
	uint64_t        total_size;
	uint32_t        sector_size;
	bool            read_only;
	bool            sync_mode;

	pthread_mutex_t lock;
	uint64_t        reads_count;
	uint64_t        writes_count;
	uint64_t        bytes_read;
	uint64_t        bytes_written;
	uint64_t        errors_count;
};

struct homa_zvol_pool {
	int              num_zvols;
	struct homa_zvol zvols[HOMA_MAX_TARGETS];
	pthread_mutex_t  pool_lock;
};

int               homa_zvol_pool_init(struct homa_zvol_pool *pool, const struct homa_config *cfg);
void              homa_zvol_pool_destroy(struct homa_zvol_pool *pool);
struct homa_zvol *homa_zvol_find(struct homa_zvol_pool *pool, const char *name);

int  homa_zvol_read(struct homa_zvol *zv, uint64_t offset, uint32_t len, void *buf);
int  homa_zvol_write(struct homa_zvol *zv, uint64_t offset, uint32_t len, const void *buf);
int  homa_zvol_flush(struct homa_zvol *zv);
int  homa_zvol_identify(struct homa_zvol *zv, uint64_t *out_size, uint32_t *out_blksize);

#endif /* _HOMA_ZVOL_H_ */
