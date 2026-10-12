#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "../homa_protocol.h"
#include "../homa_storage_proto.h"
#include "../homa_config.h"
#include "../homa_zvol.h"
#include "../homa_transport.h"

static struct homa_config g_cfg;
static struct homa_zvol_pool g_pool;
static struct homa_transport g_server_trans;
static volatile int g_server_running = 1;

static void *
server_thread(void *arg)
{
	(void)arg;
	while (g_server_running) {
		struct sockaddr_in client_addr;
		uint64_t rpc_id;
		uint8_t flags;
		void *req_buf = NULL;
		uint32_t req_len = 0;
		int rc;

		rc = homa_transport_recv_rpc(&g_server_trans, &client_addr, &rpc_id, &flags,
		    &req_buf, &req_len, 50);
		if (rc == 0 && req_buf != NULL) {
			const struct homa_blk_req *req = (const struct homa_blk_req *)req_buf;
			struct homa_blk_resp resp;
			struct homa_zvol *zv = homa_zvol_find(&g_pool, req->vol_name);

			memset(&resp, 0, sizeof(resp));
			resp.magic = HOMA_BLK_MAGIC;
			resp.version = HOMA_BLK_VERSION;

			if (zv == NULL) {
				resp.status = -19;
				homa_transport_send_rpc(&g_server_trans, &client_addr, rpc_id,
				    HOMA_FLAG_FROM_SERVER, &resp, sizeof(resp), 2000);
			} else if (req->opcode == HOMA_BLK_OP_WRITE) {
				const void *payload = (const char *)req_buf + sizeof(*req);
				resp.status = homa_zvol_write(zv, req->offset, req->length, payload);
				resp.length = req->length;
				homa_transport_send_rpc(&g_server_trans, &client_addr, rpc_id,
				    HOMA_FLAG_FROM_SERVER, &resp, sizeof(resp), 2000);
			} else if (req->opcode == HOMA_BLK_OP_READ) {
				uint32_t rlen = sizeof(resp) + req->length;
				char *rbuf = malloc(rlen);
				resp.status = homa_zvol_read(zv, req->offset, req->length, rbuf + sizeof(resp));
				resp.length = (resp.status == 0) ? req->length : 0;
				memcpy(rbuf, &resp, sizeof(resp));
				homa_transport_send_rpc(&g_server_trans, &client_addr, rpc_id,
				    HOMA_FLAG_FROM_SERVER, rbuf, rlen, 2000);
				free(rbuf);
			} else if (req->opcode == HOMA_BLK_OP_IDENTIFY) {
				uint64_t tsize = 0;
				uint32_t bsize = 0;
				resp.status = homa_zvol_identify(zv, &tsize, &bsize);
				resp.total_size = tsize;
				resp.block_size = bsize;
				homa_transport_send_rpc(&g_server_trans, &client_addr, rpc_id,
				    HOMA_FLAG_FROM_SERVER, &resp, sizeof(resp), 2000);
			}
			free(req_buf);
		}
	}
	return (NULL);
}

int
main(void)
{
	pthread_t tid;
	struct homa_transport client_trans;
	struct sockaddr_in srv_addr;

	printf("=== Running Homa Protocol & Zvol Target Self-Test ===\n");

	if (homa_config_load("homa_target.yaml", &g_cfg) != 0) {
		fprintf(stderr, "Failed to load homa_target.yaml\n");
		return (1);
	}

	g_cfg.port = 9998;
	if (homa_zvol_pool_init(&g_pool, &g_cfg) != 0) {
		fprintf(stderr, "Failed to init zvol pool\n");
		return (1);
	}

	if (homa_transport_init(&g_server_trans, "127.0.0.1", 9998, &g_cfg) != 0) {
		fprintf(stderr, "Failed to init server transport\n");
		return (1);
	}

	pthread_create(&tid, NULL, server_thread, NULL);

	if (homa_transport_init(&client_trans, "127.0.0.1", 0, &g_cfg) != 0) {
		fprintf(stderr, "Failed to init client transport\n");
		return (1);
	}

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_port = htons(9998);
	inet_pton(AF_INET, "127.0.0.1", &srv_addr.sin_addr);

	/* Test 1: Identify target */
	printf("[1] Testing IDENTIFY via Homa RPC...\n");
	struct homa_blk_req id_req;
	memset(&id_req, 0, sizeof(id_req));
	id_req.magic = HOMA_BLK_MAGIC;
	id_req.version = HOMA_BLK_VERSION;
	id_req.opcode = HOMA_BLK_OP_IDENTIFY;
	strncpy(id_req.vol_name, "zvol0", sizeof(id_req.vol_name) - 1);

	uint64_t rpc_id = homa_next_rpc_id(&client_trans);
	void *resp_buf = NULL;
	uint32_t resp_len = 0;
	struct sockaddr_in from;
	uint64_t from_id;
	uint8_t flags;

	homa_transport_send_rpc(&client_trans, &srv_addr, rpc_id, HOMA_FLAG_FROM_CLIENT,
	    &id_req, sizeof(id_req), 2000);
	int rc = homa_transport_recv_rpc(&client_trans, &from, &from_id, &flags,
	    &resp_buf, &resp_len, 2000);
	assert(rc == 0);
	struct homa_blk_resp *r = (struct homa_blk_resp *)resp_buf;
	assert(r->status == 0);
	printf("    Success! Total size: %ju bytes, Block size: %u bytes\n",
	    (uintmax_t)r->total_size, r->block_size);
	free(resp_buf);

	/* Test 2: Large Write (64 KB > 16 KB unscheduled, exercises HOMA_GRANT SRPT scheduler!) */
	printf("[2] Testing Multi-Packet WRITE (64 KB) with Homa SRPT Grants...\n");
	uint32_t payload_len = 65536;
	uint32_t wreq_len = sizeof(struct homa_blk_req) + payload_len;
	char *wbuf = malloc(wreq_len);
	struct homa_blk_req *wreq = (struct homa_blk_req *)wbuf;
	memset(wreq, 0, sizeof(*wreq));
	wreq->magic = HOMA_BLK_MAGIC;
	wreq->version = HOMA_BLK_VERSION;
	wreq->opcode = HOMA_BLK_OP_WRITE;
	strncpy(wreq->vol_name, "zvol0", sizeof(wreq->vol_name) - 1);
	wreq->offset = 4096;
	wreq->length = payload_len;

	char *payload = wbuf + sizeof(*wreq);
	for (uint32_t i = 0; i < payload_len; i++)
		payload[i] = (char)((i % 95) + 32);

	rpc_id = homa_next_rpc_id(&client_trans);
	homa_transport_send_rpc(&client_trans, &srv_addr, rpc_id, HOMA_FLAG_FROM_CLIENT,
	    wbuf, wreq_len, 3000);
	rc = homa_transport_recv_rpc(&client_trans, &from, &from_id, &flags,
	    &resp_buf, &resp_len, 3000);
	assert(rc == 0);
	r = (struct homa_blk_resp *)resp_buf;
	assert(r->status == 0);
	printf("    Success! Written %u bytes at offset %ju\n", r->length, (uintmax_t)wreq->offset);
	free(resp_buf);

	/* Test 3: Large Read (64 KB) and Byte-for-Byte Verification */
	printf("[3] Testing Multi-Packet READ (64 KB) with Homa SRPT Grants...\n");
	struct homa_blk_req rreq;
	memset(&rreq, 0, sizeof(rreq));
	rreq.magic = HOMA_BLK_MAGIC;
	rreq.version = HOMA_BLK_VERSION;
	rreq.opcode = HOMA_BLK_OP_READ;
	strncpy(rreq.vol_name, "zvol0", sizeof(rreq.vol_name) - 1);
	rreq.offset = 4096;
	rreq.length = payload_len;

	rpc_id = homa_next_rpc_id(&client_trans);
	homa_transport_send_rpc(&client_trans, &srv_addr, rpc_id, HOMA_FLAG_FROM_CLIENT,
	    &rreq, sizeof(rreq), 3000);
	rc = homa_transport_recv_rpc(&client_trans, &from, &from_id, &flags,
	    &resp_buf, &resp_len, 3000);
	assert(rc == 0);
	r = (struct homa_blk_resp *)resp_buf;
	assert(r->status == 0);
	assert(r->length == payload_len);

	const char *rdata = (const char *)resp_buf + sizeof(*r);
	int cmp = memcmp(payload, rdata, payload_len);
	assert(cmp == 0);
	printf("    Success! Data read matches sent payload byte-for-byte!\n");
	free(resp_buf);
	free(wbuf);

	printf("=== All Homa Protocol & Zvol Target Tests Passed! ===\n");

	g_server_running = 0;
	pthread_join(tid, NULL);
	homa_transport_close(&client_trans);
	homa_transport_close(&g_server_trans);
	homa_zvol_pool_destroy(&g_pool);

	return (0);
}
