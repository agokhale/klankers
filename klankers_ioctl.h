#ifndef _KLANKERS_IOCTL_H_
#define _KLANKERS_IOCTL_H_

#include <sys/ioccom.h>

struct klankers_stats {
	unsigned long bytes_read;
	unsigned long bytes_written;
	unsigned int  open_count;
	unsigned int  buffer_capacity;
	unsigned int  buffer_used;
};

#define KLANKERS_IOC_MAGIC      'k'
#define KLANKERS_IOC_RESET      _IO(KLANKERS_IOC_MAGIC, 1)
#define KLANKERS_IOC_GET_STATS  _IOR(KLANKERS_IOC_MAGIC, 2, struct klankers_stats)

#endif /* _KLANKERS_IOCTL_H_ */
