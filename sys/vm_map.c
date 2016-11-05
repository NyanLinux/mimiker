#include <stdc.h>
#include <malloc.h>
#include <pmap.h>
#include <sync.h>
#include <thread.h>
#include <vm.h>
#include <vm_pager.h>
#include <vm_object.h>
#include <vm_map.h>

static vm_map_t kspace;

void vm_map_activate(vm_map_t *map) {
  cs_enter();
  thread_self()->td_uspace = map;
  pmap_activate(map ? map->pmap : NULL);
  cs_leave();
}

vm_map_t *get_user_vm_map() { return thread_self()->td_uspace; }
vm_map_t *get_kernel_vm_map() { return &kspace; }

static bool in_range(vm_map_t *map, vm_addr_t addr) {
  return map && (map->pmap->start <= addr && addr < map->pmap->end);
}

vm_map_t *get_active_vm_map_by_addr(vm_addr_t addr) {
  if (in_range(get_user_vm_map(), addr))
      return get_user_vm_map();
  if (in_range(get_kernel_vm_map(), addr))
      return get_kernel_vm_map();
  return NULL;
}

static inline int vm_map_entry_cmp(vm_map_entry_t *a, vm_map_entry_t *b) {
  if (a->start < b->start)
    return -1;
  return a->start - b->start;
}

SPLAY_PROTOTYPE(vm_map_tree, vm_map_entry, map_tree, vm_map_entry_cmp);
SPLAY_GENERATE(vm_map_tree, vm_map_entry, map_tree, vm_map_entry_cmp);

static void vm_map_setup(vm_map_t *map) {
  TAILQ_INIT(&map->list);
  SPLAY_INIT(&map->tree);
}

static MALLOC_DEFINE(mpool, "vm_map memory pool");

void vm_map_init() {
  vm_page_t *pg = pm_alloc(2);
  kmalloc_init(mpool);
  kmalloc_add_arena(mpool, pg->vaddr, PG_SIZE(pg));

  vm_map_setup(&kspace);
  kspace.pmap = get_kernel_pmap();
}

vm_map_t *vm_map_new() {
  vm_map_t *map = kmalloc(mpool, sizeof(vm_map_t), M_ZERO);

  vm_map_setup(map);
  map->pmap = pmap_new();
  return map;
}

static bool vm_map_insert_entry(vm_map_t *vm_map, vm_map_entry_t *entry) {
  if (!SPLAY_INSERT(vm_map_tree, &vm_map->tree, entry)) {
    vm_map_entry_t *next = SPLAY_NEXT(vm_map_tree, &vm_map->tree, entry);
    if (next)
      TAILQ_INSERT_BEFORE(next, entry, map_list);
    else
      TAILQ_INSERT_TAIL(&vm_map->list, entry, map_list);
    vm_map->nentries++;
    return true;
  }
  return false;
}

vm_map_entry_t *vm_map_find_entry(vm_map_t *vm_map, vm_addr_t vaddr) {
  vm_map_entry_t *etr_it;
  TAILQ_FOREACH (etr_it, &vm_map->list, map_list)
    if (etr_it->start <= vaddr && vaddr < etr_it->end)
      return etr_it;
  return NULL;
}

static void vm_map_remove_entry(vm_map_t *vm_map, vm_map_entry_t *entry) {
  vm_map->nentries--;
  vm_object_free(entry->object);
  TAILQ_REMOVE(&vm_map->list, entry, map_list);
  kfree(mpool, entry);
}

void vm_map_delete(vm_map_t *map) {
  while (map->nentries > 0)
    vm_map_remove_entry(map, TAILQ_FIRST(&map->list));
  kfree(mpool, map);
}

vm_map_entry_t *vm_map_add_entry(vm_map_t *map, vm_addr_t start, vm_addr_t end,
                                 vm_prot_t prot) {
  assert(start >= map->pmap->start);
  assert(end <= map->pmap->end);
  assert(is_aligned(start, PAGESIZE));
  assert(is_aligned(end, PAGESIZE));

#if 0
  assert(vm_map_find_entry(map, start) == NULL);
  assert(vm_map_find_entry(map, end) == NULL);
#endif

  vm_map_entry_t *entry = kmalloc(mpool, sizeof(vm_map_entry_t), M_ZERO);

  entry->start = start;
  entry->end = end;
  entry->prot = prot;

  vm_map_insert_entry(map, entry);
  return entry;
}

/* TODO: not implemented */
void vm_map_protect(vm_map_t *map, vm_addr_t start, vm_addr_t end,
                    vm_prot_t prot) {
}

void vm_map_dump(vm_map_t *map) {
  vm_map_entry_t *it;
  kprintf("[vm_map] Virtual memory map (%08lx - %08lx):\n",
          map->pmap->start, map->pmap->end);
  TAILQ_FOREACH (it, &map->list, map_list) {
    kprintf("[vm_map] * %08lx - %08lx [%c%c%c]\n", it->start, it->end,
            (it->prot & VM_PROT_READ) ? 'r' : '-',
            (it->prot & VM_PROT_WRITE) ? 'w' : '-',
            (it->prot & VM_PROT_EXEC) ? 'x' : '-');
    vm_map_object_dump(it->object);
  }
}

void vm_page_fault(vm_map_t *map, vm_addr_t fault_addr, vm_prot_t fault_type) {
  vm_map_entry_t *entry;

  log("Page fault!");

  if (!(entry = vm_map_find_entry(map, fault_addr)))
    panic("Tried to access unmapped memory region: 0x%08lx!\n", fault_addr);

  if (entry->prot == VM_PROT_NONE)
    panic("Cannot access to address: 0x%08lx\n", fault_addr);

  if (!(entry->prot & VM_PROT_WRITE) && (fault_type == VM_PROT_WRITE))
    panic("Cannot write to address: 0x%08lx\n", fault_addr);

  if (!(entry->prot & VM_PROT_READ) && (fault_type == VM_PROT_READ))
    panic("Cannot read from address: 0x%08lx\n", fault_addr);

  assert(entry->start <= fault_addr && fault_addr < entry->end);

  vm_object_t *obj = entry->object;

  assert(obj != NULL);

  vm_addr_t fault_page = fault_addr & -PAGESIZE;
  vm_addr_t offset = fault_page - entry->start;
  vm_page_t *frame = vm_object_find_page(entry->object, offset);

  if (!frame)
    frame = obj->pgr->pgr_fault(obj, fault_page, offset, fault_type);
  pmap_map(map->pmap, fault_addr, fault_addr + PAGESIZE, frame->paddr,
           entry->prot);
}