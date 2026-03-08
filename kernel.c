#include <stdint.h>

/* ── helpers ────────────────────────────────────────────────────────── */

static inline void halt_cleanly(void) {
  __asm__ volatile("mov w0, #0x0008\n"
                   "movk w0, #0x8400, lsl #16\n"
                   "hvc #0\n");
  while (1)
    __asm__ volatile("wfi");
}

void *memset(void *s, int c, unsigned long n) {
  unsigned char *p = (unsigned char *)s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

static uint32_t be32(const void *p) {
  const uint8_t *b = (const uint8_t *)p;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int streq(const char *a, const char *b) {
  while (*a && (*a == *b)) { a++; b++; }
  return *a == *b;
}
static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static inline void mmio_w8(uint64_t a, uint8_t v)   { *(volatile uint8_t  *)a = v; }
static inline void mmio_w16(uint64_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint8_t  mmio_r8(uint64_t a)  { return *(volatile uint8_t  *)a; }
static inline uint16_t mmio_r16(uint64_t a) { return *(volatile uint16_t *)a; }
static inline uint32_t mmio_r32(uint64_t a) { return *(volatile uint32_t *)a; }

/* Flush D-cache range before DMA on bare-metal ARM64 */
static void dcache_flush(void *addr, uint64_t size) {
  uint64_t a = (uint64_t)addr & ~63ULL;
  uint64_t end = (uint64_t)addr + size;
  for (; a < end; a += 64)
    __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

/* ── FDT parsing ───────────────────────────────────────────────────── */

static uint64_t pci_ecam_base = 0;
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
  uint64_t current_reg = 0;
  int is_pci = 0;
  const uint8_t *saved_ranges = 0;
  uint32_t saved_ranges_len = 0;

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

      if (is_pci && saved_ranges && saved_ranges_len > 0 && pci_mmio32_base == 0) {
        int pac = ac_stack[depth > 0 ? depth - 1 : 0];
        int entry_bytes = (3 + pac + 2) * 4;
        for (uint32_t off = 0; off + entry_bytes <= saved_ranges_len; off += entry_bytes) {
          const uint8_t *e = saved_ranges + off;
          uint32_t phys_hi = be32(e);
          int space = (phys_hi >> 24) & 0x3;
          const uint8_t *pa = e + 12;
          uint64_t cpu_addr = (pac == 2)
            ? (((uint64_t)be32(pa) << 32) | be32(pa + 4))
            : be32(pa);
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
      uint32_t len = be32(ptr); ptr += 4;
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
        saved_ranges = ptr;
        saved_ranges_len = len;
      }
      ptr += (len + 3) & ~3;

    } else if (token == 9) {
      break;
    }
  }
}

/* ── VirtIO PCI modern ─────────────────────────────────────────────── */

static volatile uint8_t *common_cfg = 0;
static volatile uint8_t *notify_cfg = 0;
static volatile uint8_t *device_cfg = 0;
static uint32_t notify_off_mult = 0;
static uint16_t tx_notify_off   = 0;

/*
 * QSIZ must be >= 256 because Apple VZ uses queue size 256.
 * A limit of 64 causes setup_queue to silently return early.
 */
#define QSIZ 256

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};
struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[QSIZ];
  uint16_t used_event;
};
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[QSIZ];
  uint16_t avail_event;
};

static struct virtq_desc  rx_desc[QSIZ] __attribute__((aligned(4096)));
static struct virtq_avail rx_avail      __attribute__((aligned(4096)));
static struct virtq_used  rx_used       __attribute__((aligned(4096)));
static uint8_t            rx_buf[256]   __attribute__((aligned(64)));

static struct virtq_desc  tx_desc[QSIZ] __attribute__((aligned(4096)));
static struct virtq_avail tx_avail      __attribute__((aligned(4096)));
static struct virtq_used  tx_used       __attribute__((aligned(4096)));
static uint8_t            tx_buf[256]   __attribute__((aligned(64)));

static void setup_queue(uint16_t qi, struct virtq_desc *desc,
                        struct virtq_avail *avail, struct virtq_used *used) {
  uint64_t c = (uint64_t)common_cfg;
  mmio_w16(c + 0x16, qi);             /* queue_select */
  uint16_t qsz = mmio_r16(c + 0x18); /* queue_size */
  if (qsz == 0 || qsz > QSIZ) return;

  memset(desc,  0, sizeof(struct virtq_desc) * qsz);
  memset(avail, 0, sizeof(*avail));
  memset(used,  0, sizeof(*used));

  uint64_t da = (uint64_t)desc, aa = (uint64_t)avail, ua = (uint64_t)used;
  mmio_w32(c + 0x20, (uint32_t)da);        /* queue_desc lo */
  mmio_w32(c + 0x24, (uint32_t)(da >> 32));
  mmio_w32(c + 0x28, (uint32_t)aa);        /* queue_avail lo */
  mmio_w32(c + 0x2C, (uint32_t)(aa >> 32));
  mmio_w32(c + 0x30, (uint32_t)ua);        /* queue_used lo */
  mmio_w32(c + 0x34, (uint32_t)(ua >> 32));
  mmio_w16(c + 0x1C, 1);                   /* queue_enable */
}

/* ── Entry point ───────────────────────────────────────────────────── */

void kernel_main(void *fdt) {
  parse_fdt(fdt);

  if (!pci_ecam_base || !pci_mmio32_base) halt_cleanly();

  /* Find VirtIO console device (vid=0x1af4, did=0x1043 or 0x1003) */
  uint64_t dbase = 0;
  for (uint32_t d = 0; d < 32; d++) {
    uint64_t db = pci_ecam_base + ((uint64_t)d << 15);
    uint32_t id = mmio_r32(db);
    if (id == 0 || id == 0xFFFFFFFF) continue;
    uint16_t vid = id & 0xFFFF;
    uint16_t did = (id >> 16) & 0xFFFF;
    if (vid == 0x1af4 && (did == 0x1043 || did == 0x1003)) {
      dbase = db;
      break;
    }
  }
  if (!dbase) halt_cleanly();

  /*
   * CRITICAL: Enable Memory Space + Bus Master via 16-bit write only.
   * A 32-bit write to offset 0x04 also touches the Status register
   * (upper 16 bits) which crashes Apple VZ with an Internal Virtualization error.
   */
  mmio_w16(dbase + 0x04, mmio_r16(dbase + 0x04) | 0x06);

  /*
   * Apple VZ does NOT pre-program BARs (only type bits are set; address = 0).
   * We must write a valid address from the PCI MMIO window to BAR0.
   * All VirtIO caps use bar_idx=0.
   */
  uint32_t bar0_addr = (uint32_t)pci_mmio32_base;
  mmio_w32(dbase + 0x10, bar0_addr);
  mmio_w32(dbase + 0x14, 0); /* upper 32 bits for 64-bit BAR */

  /* Walk PCI capabilities to find VirtIO modern config structures */
  uint8_t cp = mmio_r8(dbase + 0x34);
  while (cp) {
    uint8_t cid   = mmio_r8(dbase + cp);
    uint8_t cnext = mmio_r8(dbase + cp + 1);
    uint8_t ctype = mmio_r8(dbase + cp + 3);
    if (cid == 0x09) { /* VirtIO vendor-specific PCI capability */
      uint32_t offset = mmio_r32(dbase + cp + 8);
      uint64_t p = (uint64_t)bar0_addr + offset;
      if      (ctype == 1) common_cfg = (volatile uint8_t *)p;
      else if (ctype == 2) {
        notify_cfg = (volatile uint8_t *)p;
        notify_off_mult = mmio_r32(dbase + cp + 0x10);
      }
      else if (ctype == 4) device_cfg = (volatile uint8_t *)p;
    }
    cp = cnext;
  }

  if (!common_cfg || !notify_cfg) halt_cleanly();

  uint64_t c = (uint64_t)common_cfg;

  /* VirtIO modern initialization */
  mmio_w8(c + 0x14, 0);  /* RESET */
  mmio_w8(c + 0x14, 1);  /* ACKNOWLEDGE */
  mmio_w8(c + 0x14, 3);  /* DRIVER */

  /* Negotiate VERSION_1 only */
  mmio_w32(c + 0x00, 0); /* device_feature_select = page 0 */
  mmio_w32(c + 0x08, 0);
  mmio_w32(c + 0x0C, 0); /* no page-0 driver features */
  mmio_w32(c + 0x08, 1);
  mmio_w32(c + 0x0C, 1); /* VERSION_1 (bit 32 = page-1 bit 0) */

  mmio_w8(c + 0x14, 11); /* FEATURES_OK */
  if (!(mmio_r8(c + 0x14) & 8)) halt_cleanly(); /* rejected */

  /* Set up RX queue (queue 0) */
  setup_queue(0, rx_desc, &rx_avail, &rx_used);
  uint16_t rx_noff = mmio_r16(c + 0x1E);
  rx_desc[0].addr  = (uint64_t)rx_buf;
  rx_desc[0].len   = sizeof(rx_buf);
  rx_desc[0].flags = 2; /* VIRTQ_DESC_F_WRITE */
  rx_avail.ring[0] = 0;
  rx_avail.idx     = 1;

  /* Set up TX queue (queue 1) */
  setup_queue(1, tx_desc, &tx_avail, &tx_used);
  tx_notify_off = mmio_r16(c + 0x1E);

  /* Flush caches for DMA coherency before signaling DRIVER_OK */
  dcache_flush(rx_desc, sizeof(rx_desc));
  dcache_flush(&rx_avail, sizeof(rx_avail));
  dcache_flush(rx_buf, sizeof(rx_buf));

  /* DRIVER_OK: device is ready */
  mmio_w8(c + 0x14, 15);

  /* Notify RX queue (give host a receive buffer) */
  mmio_w16((uint64_t)notify_cfg + rx_noff * notify_off_mult, 0);

  /* Small delay for host to initialize */
  for (volatile int i = 0; i < 1000000; i++) __asm__ volatile("nop");

  /* Prepare and send a message via TX queue */
  const char *msg = "Hello from bare-metal kernel on Apple VZ!\n";
  uint32_t len = 0;
  while (msg[len]) len++;
  for (uint32_t i = 0; i < len && i < sizeof(tx_buf); i++)
    tx_buf[i] = (uint8_t)msg[i];

  dcache_flush(tx_buf, sizeof(tx_buf));
  __asm__ volatile("dsb sy" ::: "memory");

  tx_desc[0].addr  = (uint64_t)tx_buf;
  tx_desc[0].len   = len;
  tx_desc[0].flags = 0; /* device reads from this buffer */
  tx_desc[0].next  = 0;
  tx_avail.ring[0] = 0;
  tx_avail.idx     = 1;

  dcache_flush(tx_desc, sizeof(tx_desc));
  dcache_flush(&tx_avail, sizeof(tx_avail));
  __asm__ volatile("dsb sy" ::: "memory");

  /* Notify TX queue (queue 1) */
  mmio_w16((uint64_t)notify_cfg + tx_notify_off * notify_off_mult, 1);

  /* Wait for host to process the output */
  for (volatile int i = 0; i < 10000000; i++) __asm__ volatile("nop");

  halt_cleanly();
}
