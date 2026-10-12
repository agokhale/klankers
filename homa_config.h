#ifndef _HOMA_CONFIG_H_
#define _HOMA_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

#define HOMA_MAX_TARGETS       32
#define HOMA_PATH_MAX          256
#define HOMA_CONFIG_DEFAULT_PATH "/usr/local/etc/homa_target.yaml"

struct homa_target_vol_config {
	char     name[64];
	char     path[HOMA_PATH_MAX];
	bool     read_only;
	bool     sync_mode;
	uint32_t block_size;
	char     fallback_path[HOMA_PATH_MAX];
	uint64_t fallback_size_bytes;
};

struct homa_config {
	char     listen_address[64];
	uint16_t port;
	int      worker_threads;

	uint32_t unscheduled_bytes;
	uint32_t max_grant_window;
	uint32_t grant_increment;
	uint32_t resend_interval_ms;

	int      num_targets;
	struct homa_target_vol_config targets[HOMA_MAX_TARGETS];
};

int  homa_config_load(const char *path, struct homa_config *cfg);
void homa_config_set_defaults(struct homa_config *cfg);
void homa_config_dump(const struct homa_config *cfg);

#endif /* _HOMA_CONFIG_H_ */
