# FreeBSD Kernel Module Project: `klankers`

A FreeBSD kernel module demonstrating a character device driver with `sysctl` and `ioctl` interfaces.

---

## Project Structure

- **`klankers.c`**: Kernel module source implementing:
  - Module lifecycle handlers (`MOD_LOAD`, `MOD_UNLOAD`, `MOD_SHUTDOWN`).
  - Character device (`/dev/klankers`) with `open`, `close`, `read`, `write`, and `ioctl`.
  - Mutex locking (`mtx`) for thread-safe operations.
  - Dynamic `sysctl` tree under `hw.klankers.*`.
- **`klankers_ioctl.h`**: Shared header defining ioctl requests (`KLANKERS_IOC_RESET`, `KLANKERS_IOC_GET_STATS`) and device data structures.
- **`Makefile`**: Build configuration utilizing FreeBSD's standard `<bsd.kmod.mk>`.
- **`test/`**: Userland test suite (`test_klankers.c` and `Makefile` using `<bsd.prog.mk>`).
- **`AGENTS.md` / `GEMINI.md`**: Project instructions and formatting rules.

---

## Building and Running

### 1. Build the Kernel Module
```sh
make
```

### 2. Load the Module into the Kernel
```sh
make load
```
*(Runs `sudo kldload -v ./klankers.ko`)*

### 3. Check Module Status
```sh
make status
```
Or check system log messages:
```sh
dmesg | tail -n 10
```

### 4. Query Sysctl Nodes
```sh
sysctl hw.klankers
```
Output:
```text
hw.klankers.debug: 1
hw.klankers.bytes_read: 0
hw.klankers.bytes_written: 0
hw.klankers.open_count: 0
```

### 5. Build and Run the Userland Test Suite
```sh
make test
```

### 6. Unload the Kernel Module
```sh
make unload
```
*(Runs `sudo kldunload -v klankers`)*

---

## Targets in Makefile

Target      | Description
:---------- | :---------------------------------------------
`all`       | Compiles `klankers.ko`
`load`      | Loads the module into the running kernel
`unload`    | Unloads the module from the kernel
`reload`    | Unloads and reloads the module
`status`    | Shows `kldstat` filtered for `klankers`
`test`      | Builds and executes the test suite in `test/`
`clean`     | Cleans module object and dependency files
