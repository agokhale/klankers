#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <libgen.h>

#include "homa_zvol.h"

static int
ensure_parent_dirs(const char *path)
{
	char tmp[HOMA_PATH_MAX];
	char *dir;
	struct stat sb;

	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	dir = dirname(tmp);

	if (stat(dir, &sb) == 0) {
		if (S_ISDIR(sb.st_mode))
			return (0);
		return (-1);
	}

	char cmd[HOMA_PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
	return (system(cmd));
}

int
homa_zvol_pool_init(struct homa_zvol_pool *pool, const struct homa_config *cfg)
{
	int i;

	memset(pool, 0, sizeof(*pool));
	pthread_mutex_init(&pool->pool_lock, NULL);

	for (i = 0; i < cfg->num_targets; i++) {
		const struct homa_target_vol_config *tc = &cfg->targets[i];
		struct homa_zvol *zv = &pool->zvols[pool->num_zvols];
		int oflags;
		off_t mediasize = 0;
		u_int sectorsize = 0;
		struct stat sb;
		const char *active_path = tc->path;

		memset(zv, 0, sizeof(*zv));
		strncpy(zv->name, tc->name, sizeof(zv->name) - 1);
		zv->read_only = tc->read_only;
		zv->sync_mode = tc->sync_mode;
		zv->sector_size = tc->block_size ? tc->block_size : 4096;
		pthread_mutex_init(&zv->lock, NULL);

		oflags = zv->read_only ? O_RDONLY : O_RDWR;

		zv->fd = open(active_path, oflags);
		if (zv->fd < 0 && tc->fallback_path[0] != '\0') {
			fprintf(stderr, "Notice: ZFS zvol path '%s' not accessible (%s). Checking fallback '%s'...\n",
			    active_path, strerror(errno), tc->fallback_path);

			active_path = tc->fallback_path;
			ensure_parent_dirs(active_path);

			zv->fd = open(active_path, oflags | O_CREAT, 0644);
			if (zv->fd < 0) {
				static char local_fallback[HOMA_PATH_MAX];
				snprintf(local_fallback, sizeof(local_fallback), "./%s.img", tc->name);
				active_path = local_fallback;
				zv->fd = open(active_path, oflags | O_CREAT, 0644);
			}

			if (zv->fd >= 0) {
				if (fstat(zv->fd, &sb) == 0 && sb.st_size == 0 && tc->fallback_size_bytes > 0) {
					if (ftruncate(zv->fd, (off_t)tc->fallback_size_bytes) == 0) {
						printf("Initialized backing image '%s' (%ju MB)\n",
						    active_path, (uintmax_t)(tc->fallback_size_bytes / (1024 * 1024)));
					}
				}
			}
		}

		if (zv->fd < 0) {
			fprintf(stderr, "Failed to open target volume '%s' (tried '%s'): %s\n",
			    tc->name, active_path, strerror(errno));
			pthread_mutex_destroy(&zv->lock);
			continue;
		}

		strncpy(zv->path, active_path, sizeof(zv->path) - 1);

		if (ioctl(zv->fd, DIOCGMEDIASIZE, &mediasize) == 0) {
			zv->total_size = (uint64_t)mediasize;
			if (ioctl(zv->fd, DIOCGSECTORSIZE, &sectorsize) == 0)
				zv->sector_size = sectorsize;
			printf("Attached ZFS zvol target '%s' [%s] (size: %ju bytes, sector: %u)\n",
			    zv->name, zv->path, (uintmax_t)zv->total_size, zv->sector_size);
		} else if (fstat(zv->fd, &sb) == 0) {
			zv->total_size = (uint64_t)sb.st_size;
			printf("Attached storage target '%s' [%s] (size: %ju bytes, block: %u)\n",
			    zv->name, zv->path, (uintmax_t)zv->total_size, zv->sector_size);
		}

		pool->num_zvols++;
	}

	if (pool->num_zvols == 0) {
		fprintf(stderr, "Warning: No storage target volumes successfully initialized!\n");
		return (-1);
	}

	return (0);
}

void
homa_zvol_pool_destroy(struct homa_zvol_pool *pool)
{
	int i;

	for (i = 0; i < pool->num_zvols; i++) {
		struct homa_zvol *zv = &pool->zvols[i];
		if (zv->fd >= 0) {
			close(zv->fd);
			zv->fd = -1;
		}
		pthread_mutex_destroy(&zv->lock);
	}

	pthread_mutex_destroy(&pool->pool_lock);
	pool->num_zvols = 0;
}

struct homa_zvol *
homa_zvol_find(struct homa_zvol_pool *pool, const char *name)
{
	int i;

	for (i = 0; i < pool->num_zvols; i++) {
		if (strcmp(pool->zvols[i].name, name) == 0)
			return (&pool->zvols[i]);
	}
	return (NULL);
}

int
homa_zvol_read(struct homa_zvol *zv, uint64_t offset, uint32_t len, void *buf)
{
	ssize_t nread;

	if (offset + len > zv->total_size && zv->total_size > 0)
		return (-EINVAL);

	nread = pread(zv->fd, buf, len, (off_t)offset);
	if (nread < 0) {
		int err = errno;
		pthread_mutex_lock(&zv->lock);
		zv->errors_count++;
		pthread_mutex_unlock(&zv->lock);
		return (-err);
	}

	if ((uint32_t)nread < len)
		memset((char *)buf + nread, 0, len - nread);

	pthread_mutex_lock(&zv->lock);
	zv->reads_count++;
	zv->bytes_read += len;
	pthread_mutex_unlock(&zv->lock);

	return (0);
}

int
homa_zvol_write(struct homa_zvol *zv, uint64_t offset, uint32_t len, const void *buf)
{
	ssize_t nwritten;

	if (zv->read_only)
		return (-EROFS);

	if (offset + len > zv->total_size && zv->total_size > 0)
		return (-ENOSPC);

	nwritten = pwrite(zv->fd, buf, len, (off_t)offset);
	if (nwritten < 0 || (uint32_t)nwritten != len) {
		int err = (nwritten < 0) ? errno : EIO;
		pthread_mutex_lock(&zv->lock);
		zv->errors_count++;
		pthread_mutex_unlock(&zv->lock);
		return (-err);
	}

	if (zv->sync_mode)
		fsync(zv->fd);

	pthread_mutex_lock(&zv->lock);
	zv->writes_count++;
	zv->bytes_written += len;
	pthread_mutex_unlock(&zv->lock);

	return (0);
}

int
homa_zvol_flush(struct homa_zvol *zv)
{
	if (fsync(zv->fd) != 0) {
		int err = errno;
		pthread_mutex_lock(&zv->lock);
		zv->errors_count++;
		pthread_mutex_unlock(&zv->lock);
		return (-err);
	}
	return (0);
}

int
homa_zvol_identify(struct homa_zvol *zv, uint64_t *out_size, uint32_t *out_blksize)
{
	if (out_size)
		*out_size = zv->total_size;
	if (out_blksize)
		*out_blksize = zv->sector_size;
	return (0);
}
