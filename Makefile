# FreeBSD Kernel Module and Homa Storage Target Makefile

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

# Userland build configuration for Homa daemon, client, and tests
USER_CC?=	cc
USER_CFLAGS+=	-Wall -Wextra -O2 -I/usr/local/include -I.
USER_LDFLAGS+=	-L/usr/local/lib -lyaml -lpthread

all: homa_targetd homa_client

homa_targetd: homa_targetd.c homa_config.c homa_zvol.c homa_transport.c homa_protocol.h homa_storage_proto.h homa_config.h homa_zvol.h homa_transport.h
	$(USER_CC) $(USER_CFLAGS) -o $@ homa_targetd.c homa_config.c homa_zvol.c homa_transport.c $(USER_LDFLAGS)

homa_client: homa_client.c homa_config.c homa_transport.c homa_protocol.h homa_storage_proto.h homa_config.h homa_transport.h
	$(USER_CC) $(USER_CFLAGS) -o $@ homa_client.c homa_config.c homa_transport.c $(USER_LDFLAGS)

test_homa: test/test_homa.c homa_config.c homa_zvol.c homa_transport.c homa_protocol.h homa_storage_proto.h homa_config.h homa_zvol.h homa_transport.h
	$(USER_CC) $(USER_CFLAGS) -o $@ test/test_homa.c homa_config.c homa_zvol.c homa_transport.c $(USER_LDFLAGS)

install-config:
	sudo install -m 0644 homa_target.yaml /usr/local/etc/homa_target.yaml

test-homa: test_homa
	./test_homa

status:
	kldstat | grep -E "(Id|${KMOD})"

test:
	$(MAKE) -C test run
	$(MAKE) test-homa
