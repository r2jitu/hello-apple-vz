/*
 * kernel.c — Bare-metal ARM64 "Hello World" for Apple Virtualization.framework
 *
 * Boots via VZLinuxBootLoader. Parses the FDT passed in x0 to locate the PCI
 * ECAM and MMIO window, then drives the VirtIO PCI console (device 0x1043)
 * using the modern (version 1) transport to print a message to the host.
 *
 * Key Apple VZ quirks discovered empirically (see progress.md for details):
 *
 *  - BARs are NOT pre-programmed; write an address from pci_mmio32_base.
 *  - PCI Command register (offset 0x04) must be written as 16-bit only.
 *    A 32-bit write also touches the Status register and crashes VZ.
 *  - Writing 0xFFFFFFFF to probe BAR sizes crashes VZ; don't do it.
 *  - VirtIO queue size on Apple VZ is 256; any QSIZ < 256 causes
 *    setup_queue() to bail early, leaving the TX queue unconfigured.
 *  - VZ processes the TX queue asynchronously. Poll tx_used.idx to confirm
 *    the host consumed the descriptor before calling PSCI SYSTEM_OFF.
 *
 * Built with Claude (https://claude.ai) — Anthropic.
 */

#include <stdint.h>

/* ── Low-level helpers ──────────────────────────────────────────────── */

/* Shut down the VM cleanly via PSCI SYSTEM_OFF (hvc #0, w0=0x84000008). */
static inline void psci_off(void) {
  __asm__ volatile(
      "mov  w0, #0x0008\n"
      "movk w0, #0x8400, lsl #16\n"
      "hvc  #0\n");
  while (1) __asm__ volatile("wfi");
}

void *memset(void *s, int c, unsigned long n) {
  unsigned char *p = (unsigned char *)s;
  while (n--) *p++ = (unsigned char)c;
  return s;
}

/* FDT is big-endian. */
static uint32_t be32(const void *p) {
  const uint8_t *b = (const uint8_t *)p;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static inline void mmio_w8 (uint64_t a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void mmio_w16(uint64_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint8_t  mmio_r8 (uint64_t a) { return *(volatile uint8_t  *)a; }
static inline uint16_t mmio_r16(uint64_t a) { return *(volatile uint16_t *)a; }
static inline uint32_t mmio_r32(uint64_t a) { return *(volatile uint32_t *)a; }

/* Flush a memory range from D-cache to RAM before a DMA transfer. */
static void dcache_flush(void *addr, uint64_t size) {
  uint64_t a   = (uint64_t)addr & ~63ULL;
  uint64_t end = (uint64_t)addr + size;
  for (; a < end; a += 64)
    __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

/* ── FDT parser ─────────────────────────────────────────────────────── */
/*
 * Walks the Flattened Device Tree to find the PCI host bridge and extract:
 *   pci_ecam_base   — base address of PCI Enhanced Configuration space
 *   pci_mmio32_base — base CPU address of the 32-bit PCI MMIO aperture
 *
 * On Apple VZ (from the actual FDT):
 *   pci { compatible = "pci-host-ecam-generic";
 *         reg = <0 0x40000000 0 0x10000000>;
 *         ranges = <... 0x2000000 0 0x50000000 0 0x50000000 ...>; }
 * So ECAM = 0x40000000, MMIO32 window starts at 0x50000000.
 */

static uint64_t pci_ecam_base   = 0;
static uint64_t pci_mmio32_base = 0;
static uint64_t pci_mmio32_size = 0;

static void parse_fdt(void *fdt) {
  if (be32(fdt) != 0xd00dfeed)
    return;

  uint32_t off_struct = be32((uint8_t *)fdt + 8);
  uint32_t totalsize  = be32((uint8_t *)fdt + 4);
  uint32_t off_str    = be32((uint8_t *)fdt + 12);

  const uint8_t *ptr     = (const uint8_t *)fdt + off_struct;
  const uint8_t *end     = (const uint8_t *)fdt + totalsize;
  const char    *strings = (const char    *)fdt + off_str;

  int depth = 0;
  int ac_stack[16] = {2};
  int sc_stack[16] = {1};
  uint64_t current_reg  = 0;
  int is_pci            = 0;
  const uint8_t *saved_ranges = 0;
  uint32_t saved_ranges_len   = 0;

  while (ptr < end) {
    uint32_t token = be32(ptr);
    ptr += 4;

    if (token == 1) { /* FDT_BEGIN_NODE */
      depth++;
      if (depth < 16) {
        ac_stack[depth] = ac_stack[depth - 1];
        sc_stack[depth] = sc_stack[depth - 1];
      }
      current_reg = 0; is_pci = 0;
      saved_ranges = 0; saved_ranges_len = 0;
      while (ptr < end && *ptr) ptr++;
      ptr++;
      ptr = (const uint8_t *)(((uint64_t)ptr + 3) & ~3ULL);

    } else if (token == 2) { /* FDT_END_NODE */
      if (is_pci && current_reg)
        pci_ecam_base = current_reg;

      /* Parse PCI 'ranges' to find the 32-bit MMIO aperture. */
      if (is_pci && saved_ranges && saved_ranges_len > 0 && pci_mmio32_base == 0) {
        int pac = ac_stack[depth > 0 ? depth - 1 : 0];
        int entry_bytes = (3 + pac + 2) * 4;
        for (uint32_t off = 0; off + entry_bytes <= saved_ranges_len; off += entry_bytes) {
          const uint8_t *e = saved_ranges + off;
          int space = (be32(e) >> 24) & 3;
          const uint8_t *pa = e + 12;
          uint64_t cpu_addr = (pac == 2)
            ? (((uint64_t)be32(pa) << 32) | be32(pa + 4)) : be32(pa);
          const uint8_t *sa = pa + pac * 4;
          uint64_t sz = ((uint64_t)be32(sa) << 32) | be32(sa + 4);
          if ((space == 2 || space == 3) && pci_mmio32_base == 0) {
            pci_mmio32_base = cpu_addr;
            pci_mmio32_size = sz;
          }
        }
      }
      if (depth > 0) depth--;

    } else if (token == 3) { /* FDT_PROP */
      uint32_t len     = be32(ptr); ptr += 4;
      uint32_t nameoff = be32(ptr); ptr += 4;
      const char *name = strings + nameoff;

      if (streq(name, "#address-cells") && len == 4) {
        if (depth < 16) ac_stack[depth] = be32(ptr);
      } else if (streq(name, "#size-cells") && len == 4) {
        if (depth < 16) sc_stack[depth] = be32(ptr);
      } else if (streq(name, "compatible")) {
        const char *comp = (const char *)ptr;
        for (uint32_t i = 0; i < len; i += slen(comp + i) + 1)
          if (streq(comp + i, "pci-host-ecam-generic")) is_pci = 1;
      } else if (streq(name, "reg") && len >= 4) {
        int ac = ac_stack[depth > 0 ? depth - 1 : 0];
        if (ac == 2 && len >= 8)
          current_reg = ((uint64_t)be32(ptr) << 32) | be32(ptr + 4);
        else if (ac == 1)
          current_reg = be32(ptr);
      } else if (streq(name, "ranges") && len > 0) {
        saved_ranges     = ptr;
        saved_ranges_len = len;
      }
      ptr += (len + 3) & ~3;

    } else if (token == 9) { /* FDT_END */
      break;
    }
  }
}

/* ── VirtIO PCI modern console driver ───────────────────────────────── */
/*
 * Drives the VirtIO console (vid=0x1af4 did=0x1043) via the modern PCI
 * transport. Config structure locations are advertised in vendor-specific
 * PCI capabilities (cap ID 0x09):
 *   type 1 = common_cfg  — feature negotiation, queue selection/setup
 *   type 2 = notify_cfg  — per-queue doorbell registers
 *
 * Non-multiport queue layout:
 *   Queue 0 — receiveq  (host → guest)
 *   Queue 1 — transmitq (guest → host)  ← this is what we use to print
 */

static volatile uint8_t *common_cfg = 0;
static volatile uint8_t *notify_cfg = 0;
static uint32_t notify_off_mult     = 0;
static uint16_t tx_notify_off       = 0;

/*
 * Apple VZ reports queue size = 256. QSIZ must be >= 256; otherwise
 * setup_queue() silently returns without configuring the queue.
 */
#define QSIZ 256

struct virtq_desc      { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail     { uint16_t flags; uint16_t idx; uint16_t ring[QSIZ]; uint16_t used_event; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used      { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZ]; uint16_t avail_event; };

static struct virtq_desc  rx_desc[QSIZ] __attribute__((aligned(4096)));
static struct virtq_avail rx_avail      __attribute__((aligned(4096)));
static struct virtq_used  rx_used       __attribute__((aligned(4096)));
static uint8_t            rx_buf[256]   __attribute__((aligned(64)));

static struct virtq_desc  tx_desc[QSIZ] __attribute__((aligned(4096)));
static struct virtq_avail tx_avail      __attribute__((aligned(4096)));
static struct virtq_used  tx_used       __attribute__((aligned(4096)));
static uint8_t            tx_buf[256]   __attribute__((aligned(64)));

/* Register a virtqueue with the device via common_cfg offsets. */
static void setup_queue(uint16_t qi, struct virtq_desc *desc,
                        struct virtq_avail *avail, struct virtq_used *used) {
  uint64_t c = (uint64_t)common_cfg;
  mmio_w16(c + 0x16, qi);              /* queue_select  */
  uint16_t qsz = mmio_r16(c + 0x18);  /* queue_size    */
  if (qsz == 0 || qsz > QSIZ) return;

  memset(desc,  0, sizeof(*desc) * qsz);
  memset(avail, 0, sizeof(*avail));
  memset(used,  0, sizeof(*used));

  uint64_t da = (uint64_t)desc, aa = (uint64_t)avail, ua = (uint64_t)used;
  mmio_w32(c + 0x20, (uint32_t)da);         /* queue_desc_lo  */
  mmio_w32(c + 0x24, (uint32_t)(da >> 32));
  mmio_w32(c + 0x28, (uint32_t)aa);         /* queue_avail_lo */
  mmio_w32(c + 0x2C, (uint32_t)(aa >> 32));
  mmio_w32(c + 0x30, (uint32_t)ua);         /* queue_used_lo  */
  mmio_w32(c + 0x34, (uint32_t)(ua >> 32));
  mmio_w16(c + 0x1C, 1);                    /* queue_enable   */
}

/* ── Entry point ────────────────────────────────────────────────────── */

void kernel_main(void *fdt) {
  /* 1. Locate PCI bus via device tree. */
  parse_fdt(fdt);
  if (!pci_ecam_base || !pci_mmio32_base) psci_off();

  /* 2. Find the VirtIO console PCI device (vid=0x1af4, did=0x1043/0x1003). */
  uint64_t dbase = 0;
  for (uint32_t d = 0; d < 32; d++) {
    uint64_t db = pci_ecam_base + ((uint64_t)d << 15);
    uint32_t id = mmio_r32(db);
    if (id == 0 || id == 0xFFFFFFFF) continue;
    uint16_t vid = id & 0xFFFF, did = id >> 16;
    if (vid == 0x1af4 && (did == 0x1043 || did == 0x1003)) { dbase = db; break; }
  }
  if (!dbase) psci_off();

  /*
   * 3. Enable Memory Space + Bus Master.
   *
   * IMPORTANT: use a 16-bit write to the Command register only (offset 0x04).
   * The 32-bit dword at 0x04 packs Command (bits 15:0) and Status (bits 31:16).
   * Writing 32 bits clobbers the read-only Status register, crashing Apple VZ.
   */
  mmio_w16(dbase + 0x04, mmio_r16(dbase + 0x04) | 0x06);

  /*
   * 4. Assign BAR0 from the PCI MMIO window.
   *
   * Apple VZ does not pre-program BARs — they contain only type bits with
   * the address portion zeroed. We write pci_mmio32_base as the BAR address.
   * All VirtIO config capabilities reference bar_idx=0.
   *
   * Do NOT write 0xFFFF... to probe the BAR size — that also crashes VZ.
   */
  uint32_t bar0_addr = (uint32_t)pci_mmio32_base;
  mmio_w32(dbase + 0x10, bar0_addr); /* BAR0 low  */
  mmio_w32(dbase + 0x14, 0);         /* BAR0 high */

  /*
   * 5. Walk PCI capabilities to find VirtIO config structure addresses.
   *
   * VirtIO PCI capabilities (cap ID=0x09) each contain:
   *   offset +3  : cfg_type (1=common, 2=notify, 3=isr, 4=device)
   *   offset +8  : byte offset within BAR where the structure lives
   *   offset +16 : notify_off_multiplier (notify cap only)
   */
  for (uint8_t cp = mmio_r8(dbase + 0x34); cp; cp = mmio_r8(dbase + cp + 1)) {
    if (mmio_r8(dbase + cp) != 0x09) continue;
    uint8_t  ctype  = mmio_r8 (dbase + cp + 3);
    uint32_t offset = mmio_r32(dbase + cp + 8);
    uint64_t p      = (uint64_t)bar0_addr + offset;
    if      (ctype == 1) common_cfg = (volatile uint8_t *)p;
    else if (ctype == 2) {
      notify_cfg      = (volatile uint8_t *)p;
      notify_off_mult = mmio_r32(dbase + cp + 0x10);
    }
  }
  if (!common_cfg || !notify_cfg) psci_off();

  uint64_t c = (uint64_t)common_cfg;

  /*
   * 6. VirtIO modern initialization (VirtIO spec §3.1).
   */
  /*
   * VZ processes VirtIO status writes asynchronously. Insert short delays
   * between consecutive writes and before any read-after-write to let VZ
   * catch up. Without delays, reads may return stale pre-write values.
   */
#define VZ_DELAY() do { for (volatile int _i = 0; _i < 10000; _i++) \
                          __asm__ volatile("nop"); } while (0)

  mmio_w8(c + 0x14, 0);  VZ_DELAY(); /* RESET       */
  mmio_w8(c + 0x14, 1);  VZ_DELAY(); /* ACKNOWLEDGE */
  mmio_w8(c + 0x14, 3);  VZ_DELAY(); /* DRIVER      */

  /* Negotiate VERSION_1 (feature bit 32). No other features needed. */
  mmio_w32(c + 0x00, 0); VZ_DELAY(); /* device_feature_select = page 0 */
  mmio_w32(c + 0x08, 0); mmio_w32(c + 0x0C, 0); VZ_DELAY(); /* page 0: none    */
  mmio_w32(c + 0x08, 1); mmio_w32(c + 0x0C, 1); VZ_DELAY(); /* page 1: V1      */

  mmio_w8(c + 0x14, 11); VZ_DELAY(); /* FEATURES_OK */
  if (!(mmio_r8(c + 0x14) & 8)) psci_off();

  /* 7. Configure virtqueues. */
  setup_queue(0, rx_desc, &rx_avail, &rx_used);
  uint16_t rx_noff = mmio_r16(c + 0x1E);

  /* Post one receive buffer so the host can send data to us if needed. */
  rx_desc[0].addr  = (uint64_t)rx_buf;
  rx_desc[0].len   = sizeof(rx_buf);
  rx_desc[0].flags = 2; /* VIRTQ_DESC_F_WRITE */
  rx_avail.ring[0] = 0;
  rx_avail.idx     = 1;

  setup_queue(1, tx_desc, &tx_avail, &tx_used);
  tx_notify_off = mmio_r16(c + 0x1E);

  mmio_w8(c + 0x14, 15);  /* DRIVER_OK */

  /* Notify RX: a receive buffer is ready. */
  mmio_w16((uint64_t)notify_cfg + rx_noff * notify_off_mult, 0);

  /* Brief pause while host initialises its side. */
  for (volatile int i = 0; i < 1000000; i++) __asm__ volatile("nop");

  /*
   * 8. Send a message via the TX queue.
   */
  const char *msg = "Hello from bare-metal on Apple Virtualization.framework!\n";
  uint32_t len = 0;
  while (msg[len]) len++;
  for (uint32_t i = 0; i < len && i < sizeof(tx_buf); i++)
    tx_buf[i] = (uint8_t)msg[i];

  tx_desc[0].addr  = (uint64_t)tx_buf;
  tx_desc[0].len   = len;
  tx_desc[0].flags = 0; /* device reads from this buffer */
  tx_desc[0].next  = 0;
  tx_avail.ring[0] = 0;
  tx_avail.idx     = 1;
  __asm__ volatile("dsb sy" ::: "memory");

  /* Ring the TX doorbell. */
  mmio_w16((uint64_t)notify_cfg + tx_notify_off * notify_off_mult, 1);

  /* Busy-wait until VZ's I/O thread consumes the TX descriptor. */
  while (tx_used.idx == 0)
    __asm__ volatile("dsb sy" ::: "memory");

  psci_off();
}
