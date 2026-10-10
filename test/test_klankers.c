#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include "../klankers_ioctl.h"

#define DEV_PATH "/dev/klankers"

static int
test_sysctl(void)
{
	int debug_val = 0;
	size_t len = sizeof(debug_val);

	printf("[*] Testing sysctl 'hw.klankers.debug'...\n");
	if (sysctlbyname("hw.klankers.debug", &debug_val, &len, NULL, 0) != 0) {
		perror("sysctlbyname(hw.klankers.debug) failed");
		return (-1);
	}

	printf("    hw.klankers.debug = %d\n", debug_val);
	return (0);
}

int
main(void)
{
	int fd;
	ssize_t nwritten, nread;
	char write_buf[] = "Hello from FreeBSD userland test!";
	char read_buf[256];
	struct klankers_stats stats;

	printf("=== FreeBSD Kernel Module Test: %s ===\n", DEV_PATH);

	test_sysctl();

	printf("[*] Opening %s...\n", DEV_PATH);
	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("Failed to open " DEV_PATH);
		fprintf(stderr, "Hint: make sure the kernel module is loaded (run 'make load')\n");
		return (1);
	}

	printf("[*] Resetting device buffer via ioctl...\n");
	if (ioctl(fd, KLANKERS_IOC_RESET) < 0) {
		perror("ioctl(KLANKERS_IOC_RESET) failed");
		close(fd);
		return (1);
	}

	printf("[*] Writing test payload: \"%s\"...\n", write_buf);
	nwritten = write(fd, write_buf, strlen(write_buf));
	if (nwritten < 0) {
		perror("write() failed");
		close(fd);
		return (1);
	}
	printf("    Wrote %zd bytes successfully\n", nwritten);

	printf("[*] Querying stats via ioctl...\n");
	memset(&stats, 0, sizeof(stats));
	if (ioctl(fd, KLANKERS_IOC_GET_STATS, &stats) < 0) {
		perror("ioctl(KLANKERS_IOC_GET_STATS) failed");
		close(fd);
		return (1);
	}
	printf("    Stats: capacity=%u, used=%u, written=%lu, read=%lu, open_count=%u\n",
	    stats.buffer_capacity, stats.buffer_used,
	    stats.bytes_written, stats.bytes_read, stats.open_count);

	printf("[*] Reading back from %s...\n", DEV_PATH);
	memset(read_buf, 0, sizeof(read_buf));
	nread = pread(fd, read_buf, sizeof(read_buf) - 1, 0);
	if (nread < 0) {
		perror("pread() failed");
		close(fd);
		return (1);
	}
	read_buf[nread] = '\0';
	printf("    Read %zd bytes: \"%s\"\n", nread, read_buf);

	if (strcmp(write_buf, read_buf) == 0) {
		printf("[+] Payload verification: SUCCESS (data matches perfectly)\n");
	} else {
		fprintf(stderr, "[-] Payload verification: FAILED (mismatch)\n");
		close(fd);
		return (1);
	}

	close(fd);
	printf("=== All tests passed successfully! ===\n");
	return (0);
}
