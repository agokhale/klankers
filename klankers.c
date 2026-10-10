#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

#include "klankers_ioctl.h"

#define KLANKERS_BUFFER_SIZE  4096

static MALLOC_DEFINE(M_KLANKERS, "klankers_buf", "Buffer for klankers device");

struct klankers_softc {
	struct cdev           *sc_cdev;
	struct mtx             sc_mtx;
	char                  *sc_buffer;
	size_t                 sc_buf_size;
	size_t                 sc_buf_len;
	struct klankers_stats  sc_stats;
};

static struct klankers_softc klankers_sc;
static int klankers_debug = 1;

SYSCTL_NODE(_hw, OID_AUTO, klankers, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "klankers kernel module parameters");
SYSCTL_INT(_hw_klankers, OID_AUTO, debug, CTLFLAG_RWTUN,
    &klankers_debug, 0, "Debug message verbosity (0=off, 1=on)");
SYSCTL_ULONG(_hw_klankers, OID_AUTO, bytes_read, CTLFLAG_RD,
    &klankers_sc.sc_stats.bytes_read, 0, "Total bytes read from device");
SYSCTL_ULONG(_hw_klankers, OID_AUTO, bytes_written, CTLFLAG_RD,
    &klankers_sc.sc_stats.bytes_written, 0, "Total bytes written to device");
SYSCTL_UINT(_hw_klankers, OID_AUTO, open_count, CTLFLAG_RD,
    &klankers_sc.sc_stats.open_count, 0, "Active open count");

static d_open_t  klankers_open;
static d_close_t klankers_close;
static d_read_t  klankers_read;
static d_write_t klankers_write;
static d_ioctl_t klankers_ioctl;

static struct cdevsw klankers_cdevsw = {
	.d_version = D_VERSION,
	.d_open    = klankers_open,
	.d_close   = klankers_close,
	.d_read    = klankers_read,
	.d_write   = klankers_write,
	.d_ioctl   = klankers_ioctl,
	.d_name    = "klankers",
};

static int
klankers_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct klankers_softc *sc = &klankers_sc;

	mtx_lock(&sc->sc_mtx);
	sc->sc_stats.open_count++;
	mtx_unlock(&sc->sc_mtx);

	if (klankers_debug)
		uprintf("klankers: opened (total opens: %u)\n",
		    sc->sc_stats.open_count);

	return (0);
}

static int
klankers_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	if (klankers_debug)
		uprintf("klankers: closed\n");

	return (0);
}

static int
klankers_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct klankers_softc *sc = &klankers_sc;
	int error = 0;
	size_t amnt;

	mtx_lock(&sc->sc_mtx);

	if (uio->uio_offset >= sc->sc_buf_len) {
		mtx_unlock(&sc->sc_mtx);
		return (0);
	}

	amnt = MIN(uio->uio_resid, sc->sc_buf_len - uio->uio_offset);
	error = uiomove(sc->sc_buffer + uio->uio_offset, amnt, uio);
	if (error == 0)
		sc->sc_stats.bytes_read += amnt;

	mtx_unlock(&sc->sc_mtx);
	return (error);
}

static int
klankers_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct klankers_softc *sc = &klankers_sc;
	int error = 0;
	size_t amnt;

	mtx_lock(&sc->sc_mtx);

	if (uio->uio_offset >= sc->sc_buf_size) {
		mtx_unlock(&sc->sc_mtx);
		return (ENOSPC);
	}

	amnt = MIN(uio->uio_resid, sc->sc_buf_size - uio->uio_offset);
	error = uiomove(sc->sc_buffer + uio->uio_offset, amnt, uio);
	if (error == 0) {
		if (uio->uio_offset > sc->sc_buf_len)
			sc->sc_buf_len = uio->uio_offset;
		sc->sc_stats.bytes_written += amnt;
	}

	mtx_unlock(&sc->sc_mtx);
	return (error);
}

static int
klankers_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct klankers_softc *sc = &klankers_sc;
	int error = 0;

	switch (cmd) {
	case KLANKERS_IOC_RESET:
		mtx_lock(&sc->sc_mtx);
		memset(sc->sc_buffer, 0, sc->sc_buf_size);
		sc->sc_buf_len = 0;
		mtx_unlock(&sc->sc_mtx);
		if (klankers_debug)
			uprintf("klankers: buffer reset via ioctl\n");
		break;

	case KLANKERS_IOC_GET_STATS: {
		struct klankers_stats *st = (struct klankers_stats *)data;
		mtx_lock(&sc->sc_mtx);
		*st = sc->sc_stats;
		st->buffer_capacity = (unsigned int)sc->sc_buf_size;
		st->buffer_used = (unsigned int)sc->sc_buf_len;
		mtx_unlock(&sc->sc_mtx);
		break;
	}

	default:
		error = ENOTTY;
		break;
	}

	return (error);
}

static int
klankers_init(void)
{
	struct klankers_softc *sc = &klankers_sc;

	memset(sc, 0, sizeof(*sc));
	mtx_init(&sc->sc_mtx, "klankers_mtx", NULL, MTX_DEF);

	sc->sc_buf_size = KLANKERS_BUFFER_SIZE;
	sc->sc_buffer = malloc(sc->sc_buf_size, M_KLANKERS, M_WAITOK | M_ZERO);

	sc->sc_cdev = make_dev(&klankers_cdevsw, 0, UID_ROOT, GID_WHEEL,
	    0666, "klankers");
	if (sc->sc_cdev == NULL) {
		free(sc->sc_buffer, M_KLANKERS);
		mtx_destroy(&sc->sc_mtx);
		printf("klankers: failed to create /dev/klankers\n");
		return (ENXIO);
	}

	printf("klankers: loaded successfully (device /dev/klankers created)\n");
	return (0);
}

static int
klankers_fini(void)
{
	struct klankers_softc *sc = &klankers_sc;

	if (sc->sc_cdev != NULL)
		destroy_dev(sc->sc_cdev);

	if (sc->sc_buffer != NULL)
		free(sc->sc_buffer, M_KLANKERS);

	mtx_destroy(&sc->sc_mtx);

	printf("klankers: unloaded successfully\n");
	return (0);
}

static int
klankers_modevent(module_t mod, int type, void *data)
{
	int error = 0;

	switch (type) {
	case MOD_LOAD:
		error = klankers_init();
		break;
	case MOD_UNLOAD:
		error = klankers_fini();
		break;
	case MOD_SHUTDOWN:
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

DEV_MODULE(klankers, klankers_modevent, NULL);
MODULE_VERSION(klankers, 1);
