# Progress Tracker: Custom Apple Virtualization Kernel

## Iteration 1: Basic Boot and Hardcoded pvpanic
- **Status**: Completed.
- **Details**: Built a custom `runner.swift`. Replaced `DispatchGroup.wait()` with
  `RunLoop.main.run()` to unblock VZ callbacks. VM successfully executes PSCI
  SYSTEM_OFF via `hvc #0` with w0=0x84000008.

## Iteration 2: FDT Parsing & Virtio Scanning
- **Status**: Completed.
- **Details**: Built a robust FDT walker to dynamically identify the PCI ECAM base
  and MMIO window from the device tree passed in x0. Verified by locating the
  VirtIO PCI device (vid=0x1af4) without hardcoding.

## Iteration 3: VirtIO Console Output — WORKING
- **Status**: Completed.
- **Details**: Successfully printed `Hello from bare-metal on Apple Virtualization.framework!`
  via the VirtIO PCI console TX queue. Works reliably (3/3 consecutive runs).

---

## Apple VZ Platform: Key Findings

### Device Tree (FDT)
From the actual FDT provided by Apple VZ (ref: zhuowei's gist):
- **RAM**: starts at `0x70000000`, size `0x40000000` (1 GB)
- **GIC**: `0x10000000`
- **pvpanic-mmio**: `0x20070000`
- **PCI ECAM**: `0x40000000`, size `0x10000000`
- **PCI I/O window**: `0x6fff0000` (64 KB)
- **PCI MMIO32 window**: CPU addr `0x50000000`, size `~510 MB`
- **PCI MMIO64 window**: `0x100000000`, size `1 GB`

### VirtIO Console Device (PCI)
- **Vendor ID**: `0x1af4`, **Device ID**: `0x1043` (modern VirtIO console)
- **PCI caps**: present; cap pointer at offset 0x34 is non-zero
- **Status register**: Capabilities List bit (bit 4 of Status word) is SET
- **Queue size**: **256** (not 64 — QSIZ must be ≥ 256 or setup_queue() silently bails)
- **notify_off_mult**: 4
- All VirtIO caps use `bar_idx = 0`

### PCI Config Space Access Rules (CRITICAL)
| Operation | Safe? | Notes |
|---|---|---|
| 16-bit write to Command (offset 0x04) | ✅ | Use `mmio_w16()` only |
| 32-bit write to Command+Status dword (0x04) | ❌ | Clobbers Status → VZ crash |
| 32-bit write to BAR registers (0x10+) | ✅ | Works fine |
| Write 0xFFFFFFFF to probe BAR size | ❌ | Crashes VZ |

### BAR Allocation
- Apple VZ does **NOT** pre-program BARs (address bits = 0; only type bits set)
- Must write `pci_mmio32_base` to BAR0 (16-bit upper stays 0 for 64-bit BAR)

### VirtIO Status Write Timing (CRITICAL)
Apple VZ processes VirtIO `device_status` writes **asynchronously**. A read
immediately following a write (the `FEATURES_OK` read-back check) may return
a stale pre-write value, causing spurious `FEATURES_OK` failures. Fix: insert
a short NOP delay (~10K cycles) after each status write before any read-back.
The `VZ_DELAY()` macro in kernel.c handles this.

### VirtIO Init Quirks
- **Queue size = 256** — QSIZ must accommodate this
- **`static inline` required** on MMIO helpers — non-inline defeats volatile semantics
  under clang without -O (the -O0 default may inline small statics, but don't rely on it)
- Do NOT use `-O2` — can silently reorder volatile MMIO writes
- Apple VZ processes TX queue **asynchronously** on its I/O thread
- The guest VCPU must be in **WFI** (yielded) for VZ to schedule the I/O thread
- VZ does **not** update the TX used ring — cannot poll `tx_used.idx`

### Reliable Output Mechanism
1. Guest: TX doorbell notification → enters infinite WFI loop
2. VZ: schedules I/O thread (needs VCPU in WFI to run), processes TX, writes to pipe
3. Host runner: `readabilityHandler` captures pipe data → writes to stdout
4. Host: `stdbuf -oL timeout 3 ./runner` exits after 3 seconds
5. Wait ≥ 2 seconds before re-launching (VZ needs time to release hypervisor resources)

### Debug Signals
Throughout development, we used halt/hang/panic signals for binary-search debugging:
- **`hang()`** = `while (1) wfi` → runner times out (EXIT: 124) = we reached this point
- **`psci_off()`** = PSCI SYSTEM_OFF → runner exits 0 = we reached this point cleanly
- **pvpanic write** = write `0x01` to `0x20070000` → runner exits 1 = error signal
- **clean exit (no panic)** vs **timeout** = encode binary YES/NO at any execution point

### Working VirtIO Init Sequence
1. Parse FDT for ECAM and MMIO32 window
2. Find device (vid=0x1af4, did=0x1043/0x1003) via ECAM scan
3. Enable Mem+BM: `mmio_w16(dbase + 0x04, cmd | 0x06)` ← 16-bit only!
4. Assign BAR0: `mmio_w32(dbase + 0x10, bar0_addr)` + `mmio_w32(dbase + 0x14, 0)`
5. Walk PCI caps (while loop on cap_ptr) to find common_cfg/notify_cfg
6. Modern VirtIO init: RESET → ACK → DRIVER → negotiate VERSION_1 → FEATURES_OK
7. Setup queue 0 (RX) and queue 1 (TX); read per-queue notify offsets
8. DRIVER_OK; post RX buffer; notify RX queue
9. Fill TX buffer; update TX avail ring; notify TX queue (doorbell)
10. Enter WFI loop (yields VCPU so VZ can process TX)
