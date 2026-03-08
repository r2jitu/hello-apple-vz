# Progress Tracker: Custom Apple Virtualization Kernel

## Iteration 1: Basic Boot and Hardcoded pvpanic
- **Status**: Completed.
- **Details**: Built a custom `runner.swift`. We discovered that `DispatchGroup.wait()` was blocking the main thread, causing macOS Virtualization framework callbacks to hang. We replaced it with `RunLoop.main.run()`. The VM successfully executed the hardcoded `pvpanic = 1` and `PSCI_SYSTEM_OFF`.

## Iteration 2: FDT Parsing & Virtio Scanning
- **Status**: Completed.
- **Details**: Built a robust FDT walker to dynamically identify `pvpanic-mmio` and `pci_ecam_base`. Verified this functionality via dummy memory panic codes over the `pvpanic` interface, successfully locating the Virtio PCI device without hardcoding.

## Iteration 3: VirtIO Console Output
- **Status**: Completed.
- **Details**: **Successfully printed "Hello from bare-metal kernel on Apple VZ!" to the host terminal.** The VirtIO console output is now working.

---

## Apple VZ Platform: Key Findings

### Device Tree (FDT)
From the actual FDT provided by Apple VZ (via zhuowei's gist):
- **RAM**: starts at `0x70000000`, size `0x40000000` (1GB)
- **GIC**: `0x10000000`
- **pvpanic-mmio**: `0x20070000` (confirmed working)
- **PCI ECAM**: `0x40000000`, size `0x10000000`
- **PCI I/O window**: `0x6fff0000` (size 64KB)
- **PCI MMIO32 window**: `0x50000000` (CPU addr), size `0x1eff0000` (~510MB)
- **PCI MMIO64 window**: `0x100000000`, size `0x40000000`

### VirtIO Console Device (PCI)
- **Vendor ID**: `0x1af4`, **Device ID**: `0x1043` (modern VirtIO console)
- **PCI caps**: YES, caps pointer at offset 0x34 is non-zero
- **Status register**: Capabilities List bit (bit 4) is SET
- **Queue size**: **256** (not 64!) — must set QSIZ >= 256
- **notify_off_mult**: 4

### PCI Config Space Access Rules (CRITICAL)
- **32-bit write to offset 0x04 (Command+Status dword) CRASHES Apple VZ**
- **16-bit write to offset 0x04 (Command register only) is SAFE** → use `mmio_w16`
- **32-bit writes to BAR registers (0x10+) are SAFE**
- **Writing 0xFFFFFFFF to size BARs CRASHES Apple VZ** — cannot probe BAR size this way

### BAR Allocation
- Apple VZ does **NOT** pre-program BARs (only type bits are set, address bits = 0)
- We must assign addresses from `pci_mmio32_base` and write them to BAR registers
- All VirtIO caps use `bar_idx = 0`

### VirtIO Init Quirks
- VirtIO queue size = **256** (QSIZ must accommodate this)
- Before our fix, `setup_queue()` returned early because `qsz > QSIZ(64)`
- The TX descriptor was never consumed because the queue was never properly configured

### Output Method
- `stdbuf -oL timeout N ./runner` is needed to see output (stdout is buffered by timeout)
- Cache flushing with `dc civac` may help ensure DMA coherency (added as precaution)

### Working VirtIO Init Sequence
1. Find device via ECAM scan (vid=0x1af4, any device slot)
2. Enable Mem+BM via **16-bit write**: `mmio_w16(dbase + 0x04, cmd | 0x06)`
3. Assign BAR0 from pci_mmio32_base: write address to dbase+0x10, 0 to dbase+0x14
4. Walk PCI caps to find common_cfg (type 1), notify_cfg (type 2), device_cfg (type 4)
5. Standard modern VirtIO init: RESET → ACK → DRIVER → negotiate VERSION_1 → FEATURES_OK → setup queues → DRIVER_OK
6. Send TX data with cache flush, then notify TX queue

## Iteration 4: Next Steps
- [ ] Clean up and refactor kernel to proper stable state
- [ ] Consider MULTIPORT handshake for robust port activation
- [ ] Test with different output strings and edge cases
- [ ] Make build/run scripts easier to use
