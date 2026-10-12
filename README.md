# FreeBSD Kernel Module and Homa ZFS Storage Target Project

This repository contains:
1. **FreeBSD Kernel Module (`klankers.ko`)**: Character device driver (`/dev/klankers`) with ioctl and sysctl.
2. **Homa Protocol Engine & Storage Target (`homa_targetd`)**: Receiver-driven low-latency transport protocol serving block I/O on ZFS zvols, configured via YAML in `/usr/local/etc/homa_target.yaml`.
3. **Homa Client & Benchmark Utility (`homa_client`)**: CLI tool to inspect, read, write, and benchmark block storage over Homa RPC.

---

## Architecture Overview

### 1. Homa Transport Protocol Implementation
- **Headers & Framing (`homa_protocol.h`)**:
  - `HOMA_PACKET_DATA` (0x10): Carries message segments with 64-bit `rpc_id`, total `message_length`, `offset`, and payload.
  - `HOMA_PACKET_GRANT` (0x11): Receiver-driven flow control signaling maximum allowed offset and SRPT priority.
  - `HOMA_PACKET_RESEND` (0x12) & `HOMA_PACKET_BUSY` (0x13): Loss detection and retransmission recovery.
- **Transport Engine (`homa_transport.h`, `homa_transport.c`)**:
  - Receiver-driven SRPT (Shortest Remaining Processing Time) Grant Scheduler.
  - Paces large messages (> `unscheduled_bytes`) with dynamic grants.
  - Asynchronous packet reassembly and thread-safe RPC tracking tables.

### 2. Block Storage Protocol over Homa (`homa_storage_proto.h`)
- RPC Opcodes:
  - `HOMA_BLK_OP_READ` (0x01): Reads block range from target volume.
  - `HOMA_BLK_OP_WRITE` (0x02): Writes block payload to target volume.
  - `HOMA_BLK_OP_FLUSH` (0x03): Synchronizes disk cache (`fsync()`).
  - `HOMA_BLK_OP_IDENTIFY` (0x04): Retrieves media capacity and sector size.

### 3. ZFS Zvol Storage Engine (`homa_zvol.h`, `homa_zvol.c`)
- Direct interaction with FreeBSD ZFS volume device nodes (`/dev/zvol/<pool>/<volume>`).
- Queries volume size with `ioctl(fd, DIOCGMEDIASIZE)` and sector size with `ioctl(fd, DIOCGSECTORSIZE)`.
- Concurrent thread-safe I/O via `pread()`, `pwrite()`, and `fsync()`.
- Automated fallback backing store image support for testing without root or before live zvols are provisioned.

### 4. YAML Configuration (`/usr/local/etc/homa_target.yaml`)
- Parsed via `libyaml` in `homa_config.c`.
```yaml
server:
  listen_address: "127.0.0.1"
  port: 9999
  worker_threads: 4

homa:
  unscheduled_bytes: 16384
  max_grant_window: 65536
  grant_increment: 8192
  resend_interval_ms: 100

storage:
  targets:
    - name: "zvol0"
      path: "/dev/zvol/zroot/storage0"
      read_only: false
      sync_mode: "sync"
      block_size: 4096
      fallback_path: "./zvol0.img"
      fallback_size_mb: 64

    - name: "zvol_fast"
      path: "/dev/zvol/zroot/fast0"
      read_only: false
      sync_mode: "async"
      block_size: 4096
      fallback_path: "./zvol_fast.img"
      fallback_size_mb: 32
```

---

## Building and Running

### 1. Build All Components
```sh
make
```
This builds:
- `klankers.ko` (FreeBSD kernel module)
- `homa_targetd` (Storage target daemon)
- `homa_client` (Storage client and benchmark tool)

### 2. Install Configuration to `/usr/local/etc`
```sh
sudo make install-config
```
Copies `homa_target.yaml` to `/usr/local/etc/homa_target.yaml`.

### 3. Run the Self-Test
```sh
make test-homa
```
Executes automated end-to-end tests including IDENTIFY, multi-packet large write with SRPT grants, and large read payload verification.

### 4. Start the Homa Storage Daemon
```sh
./homa_targetd -c /usr/local/etc/homa_target.yaml
```

### 5. Using the Homa Client

- **Identify Target Volume**:
  ```sh
  ./homa_client identify zvol0
  ```
- **Write Data**:
  ```sh
  ./homa_client write zvol0 0 "Hello from FreeBSD ZFS zvol over Homa Protocol!"
  ```
- **Read Data**:
  ```sh
  ./homa_client read zvol0 0 47
  ```
- **Flush Volume**:
  ```sh
  ./homa_client flush zvol0
  ```
- **Benchmark Block I/O**:
  ```sh
  ./homa_client bench zvol0 write 4096 1000
  ./homa_client bench zvol0 read 4096 1000
  ```
