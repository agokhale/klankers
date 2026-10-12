#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#include "homa_protocol.h"
#include "homa_storage_proto.h"
#include "homa_config.h"
#include "homa_zvol.h"
#include "homa_transport.h"

static volatile sig_atomic_t g_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void
handle_storage_request(struct homa_transport *trans, const struct sockaddr_in *src,
    uint64_t rpc_id, const void *req_data, uint32_t req_len,
    struct homa_zvol_pool *pool)
{
	const struct homa_blk_req *req = (const struct homa_blk_req *)req_data;
	struct homa_zvol *zv;
	struct homa_blk_resp resp;
	char *resp_buf = NULL;
	uint32_t resp_len = sizeof(resp);

	if (req_len < sizeof(*req) || req->magic != HOMA_BLK_MAGIC) {
		fprintf(stderr, "Invalid storage request magic: 0x%08x\n", req ? req->magic : 0);
		return;
	}

	memset(&resp, 0, sizeof(resp));
	resp.magic = HOMA_BLK_MAGIC;
	resp.version = HOMA_BLK_VERSION;

	zv = homa_zvol_find(pool, req->vol_name);
	if (zv == NULL) {
		resp.status = -ENODEV;
		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    &resp, sizeof(resp), 5000);
		return;
	}

	switch (req->opcode) {
	case HOMA_BLK_OP_READ: {
		resp_len = sizeof(resp) + req->length;
		resp_buf = malloc(resp_len);
		if (resp_buf == NULL) {
			resp.status = -ENOMEM;
			homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
			    &resp, sizeof(resp), 5000);
			return;
		}

		resp.status = homa_zvol_read(zv, req->offset, req->length, resp_buf + sizeof(resp));
		resp.length = (resp.status == 0) ? req->length : 0;
		if (resp.status != 0)
			resp_len = sizeof(resp);

		memcpy(resp_buf, &resp, sizeof(resp));
		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    resp_buf, resp_len, 5000);
		free(resp_buf);
		break;
	}

	case HOMA_BLK_OP_WRITE: {
		const void *write_payload = (const char *)req_data + sizeof(*req);
		uint32_t write_len = req_len - sizeof(*req);

		if (write_len < req->length) {
			resp.status = -EINVAL;
		} else {
			resp.status = homa_zvol_write(zv, req->offset, req->length, write_payload);
			resp.length = req->length;
		}

		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    &resp, sizeof(resp), 5000);
		break;
	}

	case HOMA_BLK_OP_FLUSH: {
		resp.status = homa_zvol_flush(zv);
		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    &resp, sizeof(resp), 5000);
		break;
	}

	case HOMA_BLK_OP_IDENTIFY: {
		uint64_t tsize = 0;
		uint32_t bsize = 0;
		resp.status = homa_zvol_identify(zv, &tsize, &bsize);
		resp.total_size = tsize;
		resp.block_size = bsize;
		resp.flags = zv->read_only ? 1 : 0;
		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    &resp, sizeof(resp), 5000);
		break;
	}

	default:
		resp.status = -EOPNOTSUPP;
		homa_transport_send_rpc(trans, src, rpc_id, HOMA_FLAG_FROM_SERVER,
		    &resp, sizeof(resp), 5000);
		break;
	}
}

int
main(int argc, char *argv[])
{
	const char *config_path = HOMA_CONFIG_DEFAULT_PATH;
	struct homa_config cfg;
	struct homa_zvol_pool pool;
	struct homa_transport trans;
	int opt;

	while ((opt = getopt(argc, argv, "c:h")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'h':
		default:
			fprintf(stderr, "Usage: %s [-c config.yaml]\n", argv[0]);
			return (1);
		}
	}

	printf("=== Homa ZFS Storage Target Daemon (homad) ===\n");
	printf("Loading configuration from: %s\n", config_path);

	if (homa_config_load(config_path, &cfg) != 0) {
		fprintf(stderr, "Fatal: unable to load configuration\n");
		return (1);
	}

	homa_config_dump(&cfg);

	if (homa_zvol_pool_init(&pool, &cfg) != 0) {
		fprintf(stderr, "Fatal: failed to initialize storage targets\n");
		return (1);
	}

	if (homa_transport_init(&trans, cfg.listen_address, cfg.port, &cfg) != 0) {
		fprintf(stderr, "Fatal: failed to initialize Homa transport\n");
		homa_zvol_pool_destroy(&pool);
		return (1);
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	printf("[*] Homa storage target daemon listening on %s:%u\n",
	    cfg.listen_address, cfg.port);
	printf("[*] Ready for incoming block I/O requests. Press Ctrl+C to stop.\n");

	while (g_running) {
		struct sockaddr_in client_addr;
		uint64_t rpc_id;
		uint8_t flags;
		void *req_buf = NULL;
		uint32_t req_len = 0;
		int rc;

		rc = homa_transport_recv_rpc(&trans, &client_addr, &rpc_id, &flags,
		    &req_buf, &req_len, 250);
		if (rc == 0 && req_buf != NULL) {
			handle_storage_request(&trans, &client_addr, rpc_id,
			    req_buf, req_len, &pool);
			free(req_buf);
		}
	}

	printf("\n[*] Shutting down Homa storage daemon...\n");
	homa_transport_close(&trans);
	homa_zvol_pool_destroy(&pool);
	printf("[*] Daemon stopped cleanly.\n");

	return (0);
}
