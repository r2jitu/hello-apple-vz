# hello-apple-vz

A minimal bare-metal ARM64 kernel that boots under Apple's
[Virtualization.framework](https://developer.apple.com/documentation/virtualization)
and prints a message to the host terminal via a VirtIO PCI console.

```
▶ Building kernel...
Built kernel.bin (12417 bytes)
▶ Compiling runner...
▶ Starting VM...
Hello from bare-metal on Apple Virtualization.framework!
▶ Done.
```

> **Built with Claude** — This project was developed interactively with
> [Claude](https://claude.ai) (Anthropic), which drove the iterative
> debugging process of discovering Apple VZ's undocumented VirtIO quirks.

---

## What it does

1. **`boot.S`** — Emits a valid Linux/ARM64 Image header so `VZLinuxBootLoader`
   accepts the binary, installs an exception vector table, clears BSS, and
   calls `kernel_main`.

2. **`kernel.c`** — A single C file that:
   - Parses the FDT passed in `x0` to find the PCI ECAM base and MMIO aperture.
   - Scans the PCI bus for the VirtIO console device (`vid=0x1af4, did=0x1043/0x1003`).
   - Programs the device using the VirtIO 1.x modern PCI transport.
   - Sends a string via the virtqueue TX path, polls `tx_used.idx` for
     completion, then calls PSCI `SYSTEM_OFF`.

3. **`runner.swift`** — A Swift host process that creates a
   `VZVirtualMachineConfiguration`, attaches a VirtIO serial port wired to
   stdout, and starts the VM. When the kernel calls PSCI `SYSTEM_OFF`, VZ
   invokes `guestDidStop`, which closes the pipe write end; the runner exits
   when `readabilityHandler` observes EOF, after all output has been forwarded.

---

## Requirements

- **Apple Silicon Mac** (M1 or later)
- **macOS 13 Ventura** or later
- **Xcode Command Line Tools** (provides `clang`, `swiftc`)
- **LLVM's `ld.lld`** — install via [Homebrew](https://brew.sh): `brew install llvm`

> Make sure `ld.lld` is on your `PATH`. With Homebrew:
> ```
> export PATH="$(brew --prefix llvm)/bin:$PATH"
> ```

---

## Quick start

```sh
git clone https://github.com/r2jitu/hello-apple-vz
cd hello-apple-vz
bash run.sh
```

Or, step by step:

```sh
# 1. Build the kernel flat binary
bash build.sh

# 2. Compile and codesign the Swift runner
swiftc -framework Virtualization runner.swift -o runner
codesign --entitlements entitlements.plist --force -s - runner

# 3. Run the VM (exits cleanly when the kernel calls PSCI SYSTEM_OFF)
./runner
```

---

## Apple VZ VirtIO quirks

Apple Virtualization.framework is not QEMU. Several things behave differently:

| Topic | QEMU | Apple VZ |
|---|---|---|
| **PCI BAR allocation** | Firmware pre-programs BARs | **BARs not pre-programmed** — address bits are zero; must write a valid address before use |
| **VirtIO queue size** | Typically 64 or 128 | **256** — drivers that cap at < 256 silently skip queue setup |
| **VirtIO status writes** | Synchronous | **Asynchronous** — must delay between writes and before read-back; 85% failure rate without delays |
| **VirtIO DRIVER_OK** | Host ready immediately | **Host needs time** — must pause (~1M nops) after DRIVER_OK or TX queue never becomes active |
| **VirtIO transport** | Legacy or modern | Modern only (device ID `0x1043`) |
| **UART** | PL011 at `0x09000000` | None — VirtIO console is the only output path |

### Device tree addresses

From the FDT passed in `x0` at boot (empirical; parsed at runtime so not hardcoded):

| Region | Address | Size |
|---|---|---|
| RAM | `0x70000000` | 1 GB |
| GIC | `0x10000000` | — |
| pvpanic MMIO | `0x20070000` | — |
| PCI ECAM | `0x40000000` | 256 MB |
| PCI I/O window | `0x6fff0000` | 64 KB |
| PCI MMIO32 window | `0x50000000` | ~510 MB |
| PCI MMIO64 window | `0x100000000` | 1 GB |

### VirtIO console device

- **Vendor/Device ID**: `0x1af4` / `0x1043` (modern VirtIO console)
- **Queue size**: 256 (`QSIZ` must be ≥ 256 or `setup_queue` silently returns)
- **`notify_off_multiplier`**: 4
- All VirtIO PCI caps reference `bar_idx = 0`

### VirtIO initialisation sequence

1. Parse FDT for ECAM base and MMIO32 window.
2. Scan ECAM for device `vid=0x1af4, did=0x1043/0x1003`.
3. Enable Mem + Bus Master: `mmio_w16(dbase + 0x04, cmd | 0x06)` ← 16-bit only.
4. Determine BAR0: read existing value (mask low 4 type bits); if zero (Apple VZ), write `pci_mmio32_base` from FDT.
5. Walk PCI caps (`cap_ptr` chain) to locate `common_cfg` and `notify_cfg`.
6. Modern VirtIO init: `RESET` → `ACKNOWLEDGE` → `DRIVER` → negotiate `VERSION_1` → `FEATURES_OK`.
7. Setup queue 0 (RX); post one RX buffer into it; setup queue 1 (TX); read per-queue notify offsets.
8. `DRIVER_OK`; notify RX queue; **pause ~1M nops** (VZ host side needs time to become ready).
9. Fill TX buffer; update TX avail ring (`avail.idx = 1`); `dsb sy`.
10. Ring TX doorbell; busy-poll `tx_used.idx` until non-zero; call `psci_off`.

### Output reliability

VZ updates `tx_used.idx` and writes to the host pipe on the same I/O thread
but not necessarily in that order — the pipe write can land after the
used-ring update, and the pipe write can also land after `guestDidStop` fires.

The runner handles this by keeping `readabilityHandler` alive and exiting only
when the pipe reaches EOF (empty read). `guestDidStop` closes the runner's copy
of the write end; VZ closes its copy once the VM finishes tearing down. EOF
arrives only after both write ends are closed — by which point VZ has flushed
all pending pipe data — so no output is lost regardless of ordering.

### Not implemented (intentionally omitted for minimality)

| Feature | Why omitted | What you'd add |
|---|---|---|
| **MMU / page tables** | Memory is uncached by default (no SCTLR_EL1 set); D-cache flush not needed | `setup_mmu()` in boot.S; enable caching in SCTLR_EL1 |
| **RX polling** | Guest posts one RX buffer so the host CAN send, but never reads it back | Poll `rx_used.idx`; read `rx_buf` for host-to-guest data |
| **VirtIO feature negotiation** | Only `VERSION_1` is negotiated; all device-specific features are skipped | Read and mask `device_feature` before writing `driver_feature` |
| **PCI function scan** | Only function 0 of each device slot is checked | Add a function loop: `(d << 15) | (f << 12)` for `f` in `0..7` |
| **VirtIO multiport console** | `VIRTIO_CONSOLE_F_MULTIPORT` is not negotiated; single port only | Negotiate feature bit 1; use control queue (queue 2/3) to open named ports |

### Debugging without output

With no UART, execution state can be encoded as behaviour:

- **`psci_off()`** → runner exits 0 immediately: reached this point cleanly.
- **`while (1) wfi`** → runner hangs (kill with Ctrl-C): reached this point but did not exit.
- **pvpanic write** to `0x20070000` → runner exits with error: explicit error signal.

This allows binary-search debugging of the boot path without any output device.

---

## File overview

```
boot.S              ARM64 boot stub: Linux image header, exception vectors, BSS clear
kernel.c            Bare-metal C kernel: FDT parser + VirtIO PCI console driver
runner.swift        Swift host: Virtualization.framework VM configuration and launcher
linker.ld           LLD linker script: flat binary, BSS, 64 KB stack
build.sh            Compile kernel.c + boot.S → kernel.bin
run.sh              Full build + sign + run pipeline
entitlements.plist  com.apple.security.virtualization entitlement for the runner
```

---

## References

- [Apple Virtualization.framework documentation](https://developer.apple.com/documentation/virtualization)
- [VirtIO 1.2 specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [Apple VZ device tree (zhuowei's gist)](https://gist.github.com/zhuowei/d9871eb897d41ece0bcc5cf46c805fb2)
- [ARM64 Linux Image header format](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html)
- [PSCI specification](https://developer.arm.com/documentation/den0022)

---

## License

MIT
