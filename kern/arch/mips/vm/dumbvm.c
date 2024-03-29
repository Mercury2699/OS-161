/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
/*
 * Array of unsigned for coremap. e.g. 12011230: 3 allocations
 * If contiguously allocated, values must increase strictly by 1. Unallocated : 0.
 * Correlated address calculated by: vlo + index * PageSize
 */
static unsigned int * coremap; 
static bool coremapcreated = false;
static int virtualframes;
static paddr_t vlo;
// Page Table is defined in addrspace.h
#endif

void
vm_bootstrap(void)
{
#if OPT_A3
	paddr_t max, min;
	ram_getsize(&min, &max); 
	int allframes = (max - min) / PAGE_SIZE;
	coremap = (paddr_t *)PADDR_TO_KVADDR(min);
	min += ROUNDUP(allframes * sizeof(unsigned int), PAGE_SIZE); 
	virtualframes = (max - min) / PAGE_SIZE;
	vlo = min;
	for(int i = 0; i < virtualframes; i++){
		coremap[i] = 0;
	}
	coremapcreated = true;
#endif
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);
#if OPT_A3
	if (coremapcreated){
		int start = 0, end = 0, needed = (int) npages;
		for (int i = 0; i < virtualframes ; i++){
			find:
			if (i >= virtualframes) {
				start = -1;
				end = -1; 
				break;
			}
			if (coremap[i] == 0) {
				start = i;
				int count = 0;
				for (int j = needed; j > 0; j--, count++){
					if (i+count < virtualframes && coremap[i+count] == 0){
						continue;
					} else { 
						i += count;
						start = -1;
						goto find;
					}
				}
				end = start + npages - 1;
				break;
			} else {
				continue;
			}
		}
		if (start == -1) {
			addr = 0;// Out of Memory
		} else {
			for (int i = start; i <= end; i++){
				KASSERT(coremap[i] == 0);
			}
			for (int i = 1; i <= needed; i++){
				coremap[start+i-1] = i;
			}
			addr = vlo + start * PAGE_SIZE;
		}
	} else {
		addr = ram_stealmem(npages);
	}
#else
	addr = ram_stealmem(npages);
#endif
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
	if (coremapcreated){
		spinlock_acquire(&stealmem_lock);
		int index = (addr - MIPS_KSEG0 - vlo) / PAGE_SIZE;
		KASSERT(index < virtualframes);
		for (int i = 0; index + i < virtualframes; i++){
			coremap[index+i] = 0;
			if (index + i + 1 < virtualframes){
				if (coremap[index+i+1] == (unsigned int)i+2){
					continue;
				} else {
					break;
				}
			}
		}
		spinlock_release(&stealmem_lock);
	}
	
#else 
	/* nothing - leak the memory. */
	
	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	bool r_o = false;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
		return EFAULT;
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
#if OPT_A3
	KASSERT(as->pt.as_pbase1.address != 0);
#else
	KASSERT(as->as_pbase1 != 0);
#endif
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
#if OPT_A3
	KASSERT(as->pt.as_pbase2.address != 0);
#else
	KASSERT(as->as_pbase2 != 0);
#endif
	KASSERT(as->as_npages2 != 0);
#if OPT_A3
	KASSERT(as->pt.as_stackpbase.address != 0);
#else
	KASSERT(as->as_stackpbase != 0);
#endif
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
#if OPT_A3
	KASSERT((as->pt.as_pbase1.address & PAGE_FRAME) == as->pt.as_pbase1.address);
#else
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
#endif
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
#if OPT_A3
	KASSERT((as->pt.as_pbase2.address & PAGE_FRAME) == as->pt.as_pbase2.address);
	KASSERT((as->pt.as_stackpbase.address & PAGE_FRAME) == as->pt.as_stackpbase.address);
#else
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
#if OPT_A3
		r_o = true;
		paddr = (faultaddress - vbase1) + as->pt.as_pbase1.address;
#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
#endif
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
#if OPT_A3
		paddr = (faultaddress - vbase2) + as->pt.as_pbase2.address;
#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
#endif
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
#if OPT_A3
		paddr = (faultaddress - stackbase) + as->pt.as_stackpbase.address;
#else
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
#endif
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
#if OPT_A3
		if (r_o && as->elf_loaded)
			elo &= ~TLBLO_DIRTY;
#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	ehi = faultaddress;
	if (r_o && as->elf_loaded)
		elo &= ~TLBLO_DIRTY;
	tlb_random(ehi, elo);
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
#endif
	splx(spl);
#if OPT_A3
	return 0;
#else
	return EFAULT;
#endif
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
#if OPT_A3
	as->pt.as_pbase1.address = 0;
#else
	as->as_pbase1 = 0;
#endif
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
#if OPT_A3
	as->pt.as_pbase2.address = 0;
#else
	as->as_pbase2 = 0;
#endif
	as->as_npages2 = 0;
#if OPT_A3
	as->pt.as_stackpbase.address = 0;
#else
	as->as_stackpbase = 0;
#endif
#if OPT_A3
	as->elf_loaded = false;
#endif
	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	free_kpages(PADDR_TO_KVADDR(as->pt.as_stackpbase.address));
	free_kpages(PADDR_TO_KVADDR(as->pt.as_pbase1.address));
	free_kpages(PADDR_TO_KVADDR(as->pt.as_pbase2.address));
#endif
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	KASSERT(as->pt.as_pbase1.address == 0);
	KASSERT(as->pt.as_pbase2.address == 0);
	KASSERT(as->pt.as_stackpbase.address == 0);

	as->pt.as_pbase1.address = getppages(as->as_npages1);
	if (as->pt.as_pbase1.address == 0) {
		return ENOMEM;
	}
	as->pt.as_pbase1.framenumber = (as->pt.as_pbase1.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;

	as->pt.as_pbase2.address = getppages(as->as_npages2);
	if (as->pt.as_pbase2.address == 0) {
		return ENOMEM;
	}
	as->pt.as_pbase1.framenumber = (as->pt.as_pbase2.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;

	as->pt.as_stackpbase.address = getppages(DUMBVM_STACKPAGES);
	if (as->pt.as_stackpbase.address == 0) {
		return ENOMEM;
	}
	as->pt.as_pbase1.framenumber = (as->pt.as_stackpbase.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;
	
	as_zero_region(as->pt.as_pbase1.address, as->as_npages1);
	as_zero_region(as->pt.as_pbase2.address, as->as_npages2);
	as_zero_region(as->pt.as_stackpbase.address, DUMBVM_STACKPAGES);
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#if OPT_A3
	KASSERT(as->pt.as_stackpbase.address != 0);
#else
	KASSERT(as->as_stackpbase != 0);
#endif

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}
#if OPT_A3
	memmove((void *)PADDR_TO_KVADDR(new->pt.as_pbase1.address),
		(const void *)PADDR_TO_KVADDR(old->pt.as_pbase1.address),
		old->as_npages1*PAGE_SIZE);
	new->pt.as_pbase1.framenumber = (new->pt.as_pbase1.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;

	memmove((void *)PADDR_TO_KVADDR(new->pt.as_pbase2.address),
		(const void *)PADDR_TO_KVADDR(old->pt.as_pbase2.address),
		old->as_npages2*PAGE_SIZE);
	new->pt.as_pbase2.framenumber = (new->pt.as_pbase2.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;

	memmove((void *)PADDR_TO_KVADDR(new->pt.as_stackpbase.address),
		(const void *)PADDR_TO_KVADDR(old->pt.as_stackpbase.address),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	new->pt.as_stackpbase.framenumber = (new->pt.as_stackpbase.address - MIPS_KSEG0 - vlo) / PAGE_SIZE;
#else
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif
	*ret = new;
	return 0;
}
