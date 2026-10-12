#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <yaml.h>

#include "homa_config.h"

void
homa_config_set_defaults(struct homa_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strncpy(cfg->listen_address, "0.0.0.0", sizeof(cfg->listen_address) - 1);
	cfg->port = 9999;
	cfg->worker_threads = 4;
	cfg->unscheduled_bytes = 16384;
	cfg->max_grant_window = 65536;
	cfg->grant_increment = 8192;
	cfg->resend_interval_ms = 100;
	cfg->num_targets = 0;
}

enum parse_section {
	SEC_NONE = 0,
	SEC_SERVER,
	SEC_HOMA,
	SEC_STORAGE,
	SEC_TARGETS,
	SEC_TARGET_ITEM
};

int
homa_config_load(const char *path, struct homa_config *cfg)
{
	FILE *fh = NULL;
	yaml_parser_t parser;
	yaml_event_t event;
	int done = 0;
	enum parse_section current_sec = SEC_NONE;
	char current_key[64] = {0};
	struct homa_target_vol_config *cur_target = NULL;

	homa_config_set_defaults(cfg);

	fh = fopen(path, "rb");
	if (fh == NULL) {
		fprintf(stderr, "Failed to open config file '%s': %s\n", path, strerror(errno));
		return (-1);
	}

	if (!yaml_parser_initialize(&parser)) {
		fprintf(stderr, "Failed to initialize yaml parser\n");
		fclose(fh);
		return (-1);
	}

	yaml_parser_set_input_file(&parser, fh);

	while (!done) {
		if (!yaml_parser_parse(&parser, &event)) {
			fprintf(stderr, "YAML parser error in '%s': %s (line %zu)\n",
			    path, parser.problem, parser.problem_mark.line + 1);
			yaml_parser_delete(&parser);
			fclose(fh);
			return (-1);
		}

		switch (event.type) {
		case YAML_STREAM_END_EVENT:
			done = 1;
			break;

		case YAML_SCALAR_EVENT: {
			const char *val = (const char *)event.data.scalar.value;

			if (current_key[0] == '\0') {
				if (strcmp(val, "server") == 0) {
					current_sec = SEC_SERVER;
				} else if (strcmp(val, "homa") == 0) {
					current_sec = SEC_HOMA;
				} else if (strcmp(val, "storage") == 0) {
					current_sec = SEC_STORAGE;
				} else if (strcmp(val, "targets") == 0) {
					current_sec = SEC_TARGETS;
				} else {
					strncpy(current_key, val, sizeof(current_key) - 1);
					current_key[sizeof(current_key) - 1] = '\0';
				}
			} else {
				if (current_sec == SEC_SERVER) {
					if (strcmp(current_key, "listen_address") == 0) {
						strncpy(cfg->listen_address, val, sizeof(cfg->listen_address) - 1);
					} else if (strcmp(current_key, "port") == 0) {
						cfg->port = (uint16_t)atoi(val);
					} else if (strcmp(current_key, "worker_threads") == 0) {
						cfg->worker_threads = atoi(val);
					}
				} else if (current_sec == SEC_HOMA) {
					if (strcmp(current_key, "unscheduled_bytes") == 0) {
						cfg->unscheduled_bytes = (uint32_t)strtoul(val, NULL, 10);
					} else if (strcmp(current_key, "max_grant_window") == 0) {
						cfg->max_grant_window = (uint32_t)strtoul(val, NULL, 10);
					} else if (strcmp(current_key, "grant_increment") == 0) {
						cfg->grant_increment = (uint32_t)strtoul(val, NULL, 10);
					} else if (strcmp(current_key, "resend_interval_ms") == 0) {
						cfg->resend_interval_ms = (uint32_t)strtoul(val, NULL, 10);
					}
				} else if (current_sec == SEC_TARGET_ITEM && cur_target != NULL) {
					if (strcmp(current_key, "name") == 0) {
						strncpy(cur_target->name, val, sizeof(cur_target->name) - 1);
					} else if (strcmp(current_key, "path") == 0) {
						strncpy(cur_target->path, val, sizeof(cur_target->path) - 1);
					} else if (strcmp(current_key, "read_only") == 0) {
						cur_target->read_only = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
					} else if (strcmp(current_key, "sync_mode") == 0) {
						cur_target->sync_mode = (strcmp(val, "sync") == 0);
					} else if (strcmp(current_key, "block_size") == 0) {
						cur_target->block_size = (uint32_t)strtoul(val, NULL, 10);
					} else if (strcmp(current_key, "fallback_path") == 0) {
						strncpy(cur_target->fallback_path, val, sizeof(cur_target->fallback_path) - 1);
					} else if (strcmp(current_key, "fallback_size_mb") == 0) {
						cur_target->fallback_size_bytes = (uint64_t)strtoull(val, NULL, 10) * 1024ULL * 1024ULL;
					}
				}
				current_key[0] = '\0';
			}
			break;
		}

		case YAML_SEQUENCE_START_EVENT:
			if (current_sec == SEC_TARGETS || current_sec == SEC_STORAGE) {
				current_sec = SEC_TARGETS;
			}
			break;

		case YAML_SEQUENCE_END_EVENT:
			if (current_sec == SEC_TARGETS) {
				current_sec = SEC_NONE;
			}
			break;

		case YAML_MAPPING_START_EVENT:
			if (current_sec == SEC_TARGETS) {
				if (cfg->num_targets < HOMA_MAX_TARGETS) {
					cur_target = &cfg->targets[cfg->num_targets];
					memset(cur_target, 0, sizeof(*cur_target));
					cur_target->block_size = 4096;
					cur_target->sync_mode = true;
					cfg->num_targets++;
					current_sec = SEC_TARGET_ITEM;
				}
			}
			break;

		case YAML_MAPPING_END_EVENT:
			if (current_sec == SEC_TARGET_ITEM) {
				current_sec = SEC_TARGETS;
				cur_target = NULL;
			} else if (current_sec == SEC_SERVER || current_sec == SEC_HOMA || current_sec == SEC_STORAGE) {
				current_sec = SEC_NONE;
			}
			break;

		default:
			break;
		}

		yaml_event_delete(&event);
	}

	yaml_parser_delete(&parser);
	fclose(fh);
	return (0);
}

void
homa_config_dump(const struct homa_config *cfg)
{
	int i;

	printf("--- Homa Storage Target Configuration ---\n");
	printf("Server:\n");
	printf("  Listen Address:    %s\n", cfg->listen_address);
	printf("  Port:              %u\n", cfg->port);
	printf("  Worker Threads:    %d\n", cfg->worker_threads);
	printf("Homa Transport:\n");
	printf("  Unscheduled Bytes: %u\n", cfg->unscheduled_bytes);
	printf("  Max Grant Window:  %u\n", cfg->max_grant_window);
	printf("  Grant Increment:   %u\n", cfg->grant_increment);
	printf("  Resend Interval:   %u ms\n", cfg->resend_interval_ms);
	printf("Target Volumes (%d configured):\n", cfg->num_targets);

	for (i = 0; i < cfg->num_targets; i++) {
		const struct homa_target_vol_config *t = &cfg->targets[i];
		printf("  [%d] %s:\n", i, t->name);
		printf("      ZFS Zvol Path: %s\n", t->path);
		printf("      Read Only:     %s\n", t->read_only ? "true" : "false");
		printf("      Sync Mode:     %s\n", t->sync_mode ? "sync" : "async");
		printf("      Block Size:    %u bytes\n", t->block_size);
		if (t->fallback_path[0] != '\0') {
			printf("      Fallback Path: %s (size: %ju bytes)\n",
			    t->fallback_path, (uintmax_t)t->fallback_size_bytes);
		}
	}
	printf("-----------------------------------------\n");
}
