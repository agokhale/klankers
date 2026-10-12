#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "homa_protocol.h"
#include "homa_storage_proto.h"
#include "homa_config.h"
#include "homa_transport.h"

static uint64_t
get_monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL);
}

static int
do_rpc(struct homa_transport *trans, const struct sockaddr_in *target,
    const void *req, uint32_t req_len, void **resp, uint32_t *resp_len)
{
	uint64_t rpc_id = homa_next_rpc_id(trans);
	struct sockaddr_in reply_src;
	uint64_t reply_id;
	uint8_t flags;
	int rc;

	rc = homa_transport_send_rpc(trans, target, rpc_id, HOMA_FLAG_FROM_CLIENT,
	    req, req_len, 5000);
	if (rc != 0) {
		fprintf(stderr, "Failed to send Homa RPC %ju: %s\n", (uintmax_t)rpc_id, strerror(-rc));
		return (rc);
	}

	rc = homa_transport_recv_rpc(trans, &reply_src, &reply_id, &flags,
	    resp, resp_len, 5000);
	if (rc != 0) {
		fprintf(stderr, "Failed to receive Homa RPC reply: %s\n", strerror(-rc));
		return (rc);
	}

	return (0);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options] <command> [args...]\n"
	    "Options:\n"
	    "  -h <host>    Target host (default: 127.0.0.1)\n"
	    "  -p <port>    Target port (default: 9999)\n"
	    "Commands:\n"
	    "  identify <volume>\n"
	    "  read     <volume> <offset> <length>\n"
	    "  write    <volume> <offset> <string_data>\n"
	    "  flush    <volume>\n"
	    "  bench    <volume> <read|write> <block_size> <num_ops>\n",
	    prog);
}

int
main(int argc, char *argv[])
{
	const char *target_ip = "127.0.0.1";
	uint16_t target_port = 9999;
	struct sockaddr_in target_addr;
	struct homa_config cfg;
	struct homa_transport trans;
	int opt;

	while ((opt = getopt(argc, argv, "h:p:")) != -1) {
		switch (opt) {
		case 'h':
			target_ip = optarg;
			break;
		case 'p':
			target_port = (uint16_t)atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return (1);
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return (1);
	}

	memset(&target_addr, 0, sizeof(target_addr));
	target_addr.sin_family = AF_INET;
	target_addr.sin_port = htons(target_port);
	inet_pton(AF_INET, target_ip, &target_addr.sin_addr);

	homa_config_set_defaults(&cfg);
	if (homa_transport_init(&trans, "0.0.0.0", 0, &cfg) != 0) {
		fprintf(stderr, "Failed to initialize client Homa transport\n");
		return (1);
	}

	const char *cmd = argv[optind];

	if (strcmp(cmd, "identify") == 0) {
		if (optind + 1 >= argc) {
			fprintf(stderr, "Usage: %s identify <volume>\n", argv[0]);
			homa_transport_close(&trans);
			return (1);
		}
		const char *vol = argv[optind + 1];
		struct homa_blk_req req;
		void *resp_buf = NULL;
		uint32_t resp_len = 0;

		memset(&req, 0, sizeof(req));
		req.magic = HOMA_BLK_MAGIC;
		req.version = HOMA_BLK_VERSION;
		req.opcode = HOMA_BLK_OP_IDENTIFY;
		strncpy(req.vol_name, vol, sizeof(req.vol_name) - 1);

		if (do_rpc(&trans, &target_addr, &req, sizeof(req), &resp_buf, &resp_len) == 0) {
			struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
			if (r->status == 0) {
				printf("Target volume '%s':\n", vol);
				printf("  Total Size: %ju bytes (%ju MB)\n",
				    (uintmax_t)r->total_size, (uintmax_t)(r->total_size / (1024 * 1024)));
				printf("  Block Size: %u bytes\n", r->block_size);
				printf("  Read Only:  %s\n", (r->flags & 1) ? "true" : "false");
			} else {
				fprintf(stderr, "Identify failed: error %d (%s)\n",
				    r->status, strerror(-r->status));
			}
			free(resp_buf);
		}
	} else if (strcmp(cmd, "write") == 0) {
		if (optind + 3 >= argc) {
			fprintf(stderr, "Usage: %s write <volume> <offset> <string_data>\n", argv[0]);
			homa_transport_close(&trans);
			return (1);
		}
		const char *vol = argv[optind + 1];
		uint64_t offset = (uint64_t)strtoull(argv[optind + 2], NULL, 10);
		const char *data_str = argv[optind + 3];
		uint32_t data_len = strlen(data_str);

		uint32_t req_len = sizeof(struct homa_blk_req) + data_len;
		char *req_buf = malloc(req_len);
		struct homa_blk_req *req = (struct homa_blk_req *)req_buf;
		void *resp_buf = NULL;
		uint32_t resp_len = 0;

		memset(req, 0, sizeof(*req));
		req->magic = HOMA_BLK_MAGIC;
		req->version = HOMA_BLK_VERSION;
		req->opcode = HOMA_BLK_OP_WRITE;
		strncpy(req->vol_name, vol, sizeof(req->vol_name) - 1);
		req->offset = offset;
		req->length = data_len;
		memcpy(req_buf + sizeof(*req), data_str, data_len);

		if (do_rpc(&trans, &target_addr, req_buf, req_len, &resp_buf, &resp_len) == 0) {
			struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
			if (r->status == 0) {
				printf("[+] Write successful: %u bytes written at offset %ju\n",
				    r->length, (uintmax_t)offset);
			} else {
				fprintf(stderr, "[-] Write failed: error %d (%s)\n",
				    r->status, strerror(-r->status));
			}
			free(resp_buf);
		}
		free(req_buf);
	} else if (strcmp(cmd, "read") == 0) {
		if (optind + 3 >= argc) {
			fprintf(stderr, "Usage: %s read <volume> <offset> <length>\n", argv[0]);
			homa_transport_close(&trans);
			return (1);
		}
		const char *vol = argv[optind + 1];
		uint64_t offset = (uint64_t)strtoull(argv[optind + 2], NULL, 10);
		uint32_t length = (uint32_t)strtoul(argv[optind + 3], NULL, 10);

		struct homa_blk_req req;
		void *resp_buf = NULL;
		uint32_t resp_len = 0;

		memset(&req, 0, sizeof(req));
		req.magic = HOMA_BLK_MAGIC;
		req.version = HOMA_BLK_VERSION;
		req.opcode = HOMA_BLK_OP_READ;
		strncpy(req.vol_name, vol, sizeof(req.vol_name) - 1);
		req.offset = offset;
		req.length = length;

		if (do_rpc(&trans, &target_addr, &req, sizeof(req), &resp_buf, &resp_len) == 0) {
			struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
			if (r->status == 0 && resp_len >= sizeof(*r) + r->length) {
				const char *payload = (const char *)resp_buf + sizeof(*r);
				printf("[+] Read successful: %u bytes from offset %ju:\n",
				    r->length, (uintmax_t)offset);
				fwrite(payload, 1, r->length, stdout);
				printf("\n");
			} else {
				fprintf(stderr, "[-] Read failed: error %d (%s)\n",
				    r->status, strerror(-r->status));
			}
			free(resp_buf);
		}
	} else if (strcmp(cmd, "flush") == 0) {
		if (optind + 1 >= argc) {
			fprintf(stderr, "Usage: %s flush <volume>\n", argv[0]);
			homa_transport_close(&trans);
			return (1);
		}
		const char *vol = argv[optind + 1];
		struct homa_blk_req req;
		void *resp_buf = NULL;
		uint32_t resp_len = 0;

		memset(&req, 0, sizeof(req));
		req.magic = HOMA_BLK_MAGIC;
		req.version = HOMA_BLK_VERSION;
		req.opcode = HOMA_BLK_OP_FLUSH;
		strncpy(req.vol_name, vol, sizeof(req.vol_name) - 1);

		if (do_rpc(&trans, &target_addr, &req, sizeof(req), &resp_buf, &resp_len) == 0) {
			struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
			if (r->status == 0) {
				printf("[+] Flush successful for volume '%s'\n", vol);
			} else {
				fprintf(stderr, "[-] Flush failed: error %d (%s)\n",
				    r->status, strerror(-r->status));
			}
			free(resp_buf);
		}
	} else if (strcmp(cmd, "bench") == 0) {
		if (optind + 4 >= argc) {
			fprintf(stderr, "Usage: %s bench <volume> <read|write> <block_size> <num_ops>\n", argv[0]);
			homa_transport_close(&trans);
			return (1);
		}
		const char *vol = argv[optind + 1];
		const char *op_type = argv[optind + 2];
		uint32_t blk_size = (uint32_t)strtoul(argv[optind + 3], NULL, 10);
		int num_ops = atoi(argv[optind + 4]);
		int is_write = (strcmp(op_type, "write") == 0);

		printf("=== Benchmarking Homa Block I/O (%s, %d ops, %u bytes/op) ===\n",
		    op_type, num_ops, blk_size);

		char *buf = malloc(blk_size);
		memset(buf, 0xAB, blk_size);

		uint64_t total_us = 0;
		int success = 0;
		uint64_t t0 = get_monotonic_us();

		for (int i = 0; i < num_ops; i++) {
			uint64_t off = (uint64_t)i * blk_size;
			void *resp_buf = NULL;
			uint32_t resp_len = 0;
			int rc;

			if (is_write) {
				uint32_t req_len = sizeof(struct homa_blk_req) + blk_size;
				char *req_buf = malloc(req_len);
				struct homa_blk_req *req = (struct homa_blk_req *)req_buf;
				memset(req, 0, sizeof(*req));
				req->magic = HOMA_BLK_MAGIC;
				req->version = HOMA_BLK_VERSION;
				req->opcode = HOMA_BLK_OP_WRITE;
				strncpy(req->vol_name, vol, sizeof(req->vol_name) - 1);
				req->offset = off;
				req->length = blk_size;
				memcpy(req_buf + sizeof(*req), buf, blk_size);

				rc = do_rpc(&trans, &target_addr, req_buf, req_len, &resp_buf, &resp_len);
				free(req_buf);
			} else {
				struct homa_blk_req req;
				memset(&req, 0, sizeof(req));
				req.magic = HOMA_BLK_MAGIC;
				req.version = HOMA_BLK_VERSION;
				req.opcode = HOMA_BLK_OP_READ;
				strncpy(req.vol_name, vol, sizeof(req.vol_name) - 1);
				req.offset = off;
				req.length = blk_size;

				rc = do_rpc(&trans, &target_addr, &req, sizeof(req), &resp_buf, &resp_len);
			}

			if (rc == 0) {
				struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
				if (r->status == 0)
					success++;
				free(resp_buf);
			}
		}

		uint64_t t1 = get_monotonic_us();
		total_us = t1 - t0;
		double secs = (double)total_us / 1000000.0;
		double iops = (double)success / secs;
		double mb_sec = ((double)success * blk_size) / (secs * 1024.0 * 1024.0);
		double avg_lat_us = (double)total_us / (double)(success > 0 ? success : 1);

		printf("Results:\n");
		printf("  Successful operations: %d / %d\n", success, num_ops);
		printf("  Total time:            %.3f seconds\n", secs);
		printf("  IOPS:                  %.1f\n", iops);
		printf("  Throughput:            %.2f MB/s\n", mb_sec);
		printf("  Average Latency:       %.1f us (%.3f ms)\n", avg_lat_us, avg_lat_us / 1000.0);

		free(buf);
	} else {
		usage(argv[0]);
		homa_transport_close(&trans);
		return (1);
	}

	homa_transport_close(&trans);
	return (0);
}
