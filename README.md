# hello-apple-vz

A minimal bare-metal ARM64 kernel that boots under Apple's
[Virtualization.framework](https://developer.apple.com/documentation/virtualization)
and prints a message to the host terminal via a VirtIO PCI console.

```
▶ Building kernel...
Built kernel.bin (9372 bytes)
▶ Compiling runner...
▶ Starting VM...
Hello from bare-metal on Apple Virtualization.framework!
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
   - Scans the PCI bus for the VirtIO console device (`vid=0x1af4 did=0x1043`).
   - Programs the device using the VirtIO 1.x modern PCI transport.
   - Sends a string to the host via the virtqueue TX path.

3. **`runner.swift`** — A Swift host process that creates a
   `VZVirtualMachineConfiguration`, attaches a VirtIO serial port wired to
   stdout, and starts the VM.

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
git clone https://github.com/YOUR_USERNAME/hello-apple-vz
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

# 3. Run the VM
# stdbuf -oL forces line-buffered output; timeout 3 terminates after output arrives
# (the kernel stays in WFI indefinitely — it never calls PSCI shutdown)
stdbuf -oL timeout 3 ./runner || true
```

---

## Apple VZ VirtIO quirks

Apple Virtualization.framework is not QEMU. Several things behave differently:

| Topic | QEMU | Apple VZ |
|---|---|---|
| **PCI BAR allocation** | Firmware pre-programs BARs | **BARs are zero — you must write the address** |
| **BAR sizing** | Write 0xFFFF… to probe | **Crashes VZ — do not probe BAR sizes** |
| **PCI Command register** | 32-bit R/M/W to offset 0x04 | **Must be 16-bit write only**; 32-bit clobbers read-only Status and crashes VZ |
| **VirtIO queue size** | Typically 64 or 128 | **256** — drivers that cap at < 256 silently skip queue setup |
| **VirtIO status writes** | Synchronous | **Asynchronous** — insert a short delay before reading back `device_status` |
| **VirtIO TX processing** | Immediate or poll `tx_used.idx` | **Asynchronous on I/O thread**; VCPU must be in WFI; `tx_used.idx` is never updated |
| **VirtIO transport** | Legacy or modern | Modern only (device ID 0x1043) |
| **UART** | PL011 at 0x09000000 | None — VirtIO console is the only output path |

### Device tree addresses (empirical, may change)

| Region | Address |
|---|---|
| RAM base | `0x70000000` |
| PCI ECAM | `0x40000000` |
| PCI MMIO32 aperture | `0x50000000` |
| pvpanic MMIO | `0x20070000` |

The FDT is parsed at runtime so the kernel doesn't hard-code these.

### Debugging without output

With no UART, binary tracing through execution behavior:
- **WFI loop** → `stdbuf -oL timeout N ./runner` times out (EXIT 124): "we reached this point"
- **`psci_off()`** → runner exits 0 immediately: "we did NOT reach this point"
- **pvpanic write** to `0x20070000` → runner exits with error: explicit error signal

This lets you binary-search the execution path without any output device.

---

## File overview

```
boot.S          ARM64 boot stub: Linux image header, exception vectors, BSS clear
kernel.c        Bare-metal C kernel: FDT parser + VirtIO PCI console driver
runner.swift    Swift host: Virtualization.framework VM configuration and launcher
linker.ld       LLD linker script: flat binary, BSS, 64 KB stack
build.sh        Compile kernel.c + boot.S → kernel.bin
run.sh          Full build + sign + run pipeline
entitlements.plist  com.apple.security.virtualization entitlement for the runner
progress.md     Development log with detailed Apple VZ findings
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
