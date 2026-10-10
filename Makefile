# FreeBSD Kernel Module Makefile for klankers

KMOD=	klankers
SRCS=	klankers.c device_if.h bus_if.h

# Automatically detect kernel source directory
.if !defined(SYSDIR)
.if exists(/usr/src/freebsd/sys)
SYSDIR=	/usr/src/freebsd/sys
.elif exists(/usr/src/sys)
SYSDIR=	/usr/src/sys
.elif exists(/sys)
SYSDIR=	/sys
.endif
.endif

.include <bsd.kmod.mk>

# Convenience targets for development and testing
status:
	kldstat | grep -E "(Id|${KMOD})"

test:
	$(MAKE) -C test run
