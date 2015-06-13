
#include "fs.h"

// Return the virtual address of this disk block.
void*
diskaddr(uint32_t blockno)
{
	if (blockno == 0 || (super && blockno >= super->s_nblocks))
		panic("bad block number %08x in diskaddr", blockno);
	// here we get the correct cache block
	if (blockno < DISKCACHEOFF) {
		return (char*) (DISKMAP + blockno * BLKSIZE);
	}
	int i;

	//cprintf("request block number %d\n", blockno);
	//for (i = 0; i < DISKCACHESIZE; i++) {
	//	cprintf("cache block %3d: 0x%08x %010u %010u\n", 
	//		i, CACHEBLK2VA(i), blkcache[i].blockno, blkcache[i].count);
	//}

	int lu_cacheblk_id = 0;
	struct CacheBlock lu_cacheblk = blkcache[0];
	if (lu_cacheblk.blockno == blockno) {
		blkcache[lu_cacheblk_id].count ++;
		return (void*) CACHEBLK2VA(lu_cacheblk_id);
	}
	for (i = 1; i < DISKCACHESIZE; i++) {
		struct CacheBlock cur_cacheblk = blkcache[i];
		if (cur_cacheblk.blockno == blockno) {
			blkcache[i].count ++;
			return (void*) CACHEBLK2VA(i);
		}
		if (lu_cacheblk.count > cur_cacheblk.count) {
			lu_cacheblk_id = i;
			lu_cacheblk = cur_cacheblk;
		}
	}
	// we need to replace:
	// first flush:
	//cprintf("0x%08x\n", lu_cacheblk_id);

	flush_block(CACHEBLK2VA(lu_cacheblk_id));
	blkcache[lu_cacheblk_id].blockno = blockno;
	blkcache[lu_cacheblk_id].count = 1;
	
	return CACHEBLK2VA(lu_cacheblk_id);
	//return (char*) (DISKMAP + blockno * BLKSIZE);
}

// Is this virtual address mapped?
bool
va_is_mapped(void *va)
{
	return (uvpd[PDX(va)] & PTE_P) && (uvpt[PGNUM(va)] & PTE_P);
}

// Is this virtual address dirty?
bool
va_is_dirty(void *va)
{
	return (uvpt[PGNUM(va)] & PTE_D) != 0;
}

uint32_t cache_find_block(void *addr) {
	//cprintf("finding address 0x%08x %d\n", addr, VA2CACHEBLK(addr));
	if ((uint32_t)addr - DISKMAP < DISKCACHEOFF * BLKSIZE) {
		cprintf("%08x\n", (uint32_t)addr-DISKMAP);
		return ((uint32_t)addr - DISKMAP)/BLKSIZE;
	} else {
		//printf("got %d\n", blkcache[VA2CACHEBLK(addr)].blockno);
		return blkcache[VA2CACHEBLK(addr)].blockno;
	}
	//panic("cache_find_block: can't find addr");
}

// Fault any disk block that is read in to memory by
// loading it from disk.
static void
bc_pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t blockno = cache_find_block(addr);
	int r;
	int i;
	int sectno;
	// cprintf("bc_pgfault: addr 0x%08x\n", (uint32_t)addr);
	// Check that the fault was within the block cache region
	if (addr < (void*)DISKMAP || addr >= (void*)(DISKMAP + DISKSIZE))
		panic("page fault in FS: eip %08x, va %08x, err %04x",
		      utf->utf_eip, addr, utf->utf_err);

	// Sanity check the block number.
	if (super && blockno >= super->s_nblocks)
		panic("reading non-existent block %08x\n", blockno);

	// Allocate a page in the disk map region, read the contents
	// of the block from the disk into that page.
	// Hint: first round addr to page boundary. fs/ide.c has code to read
	// the disk.
	//
	// LAB 5: you code here:
	addr = (void *) ROUNDDOWN((uint32_t)addr, PGSIZE); // first round down to align the address
	if ((r = sys_page_alloc(0, addr, PTE_U|PTE_P|PTE_W)) < 0)
		panic("in bc_pgfault, sys_page_alloc: %e", r);

	// read sectors into pages
	sectno = blockno * BLKSECTS;
	if ((r = ide_read(sectno, addr, BLKSECTS)) < 0)
		panic("in bc_pgfault, ide_read: %e", r);

	// Clear the dirty bit for the disk block page since we just read the
	// block from disk
	if ((r = sys_page_map(0, addr, 0, addr, uvpt[PGNUM(addr)] & PTE_SYSCALL)) < 0)
		panic("in bc_pgfault, sys_page_map: %e", r);

	// Check that the block we read was allocated. (exercise for
	// the reader: why do we do this *after* reading the block
	// in?)
	if (bitmap && block_is_free(blockno))
		panic("reading free block %08x\n", blockno);
}

// Flush the contents of the block containing VA out to disk if
// necessary, then clear the PTE_D bit using sys_page_map.
// If the block is not in the block cache or is not dirty, does
// nothing.
// Hint: Use va_is_mapped, va_is_dirty, and ide_write.
// Hint: Use the PTE_SYSCALL constant when calling sys_page_map.
// Hint: Don't forget to round addr down.
void
flush_block(void *addr)
{
	uint32_t blockno = cache_find_block(addr);
	int r;
	if (addr < (void*)DISKMAP || addr >= (void*)(DISKMAP + DISKSIZE))
		panic("flush_block of bad va %08x", addr);

	// LAB 5: Your code here.
	addr = (void*) ROUNDDOWN((uint32_t)addr, PGSIZE);
	if (!va_is_mapped(addr) || !va_is_dirty(addr))
		return ; // does nothing
	if ((r = ide_write(blockno*BLKSECTS, addr, BLKSECTS)) < 0)
		panic("flush_block ide_write failed: %e", r);
	if ((r = sys_page_map(0, addr, 0, addr, uvpt[PGNUM(addr)] & PTE_SYSCALL)) < 0)
		panic("flush_block sys_page_map failed: %e", r);
}

// Test that the block cache works, by smashing the superblock and
// reading it back.
static void
check_bc(void)
{
	struct Super backup;
	cprintf("running check_bc ...\n");
	// back up super block
	memmove(&backup, diskaddr(1), sizeof backup);

	// smash it
	strcpy(diskaddr(1), "OOPS!\n");
	flush_block(diskaddr(1));
	assert(va_is_mapped(diskaddr(1)));
	assert(!va_is_dirty(diskaddr(1)));

	// clear it out
	sys_page_unmap(0, diskaddr(1));
	assert(!va_is_mapped(diskaddr(1)));

	// read it back in
	assert(strcmp(diskaddr(1), "OOPS!\n") == 0);

	// fix it
	memmove(diskaddr(1), &backup, sizeof backup);
	flush_block(diskaddr(1));

	cprintf("block cache is good\n");
}

void
bc_init(void)
{
	struct Super super;
	set_pgfault_handler(bc_pgfault);
	check_bc();

	cprintf("diskaddr: %d -> 0x%08x\n", 1, diskaddr(1));
	cprintf("blockno: %d\n", cache_find_block(diskaddr(1)));
	cprintf("sizeof super %d\n", sizeof super);
	// cache the super block by reading it once
	memmove(&super, diskaddr(1), sizeof super);
	cprintf("super has %d blocks\n", super.s_nblocks);
}

