// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock[NCPU]; //修改1：每个cpu一把锁、一个freelist
  struct run *freelist[NCPU];
} kmem;

void
kinit()
{
  for(int i=0; i < NCPU; i++)
  {
    initlock(&kmem.lock[i], "kmem");  //修改2：初始化所有锁，即使有些锁不会被用上
    kmem.freelist[i] = 0;
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();   //修改3：kfree会向当前cpu的freelist插入新的头
  int cpu_id = cpuid();
  acquire(&kmem.lock[cpu_id]);
  r->next = kmem.freelist[cpu_id];
  kmem.freelist[cpu_id] = r;
  release(&kmem.lock[cpu_id]);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem.lock[cpu_id]);
  r = kmem.freelist[cpu_id];
  if(r)                               //修改5：kal从当前cpu的freelist中分配一个空闲页面
    kmem.freelist[cpu_id] = r->next;
  else{
    for(int i=0; i < NCPU; i++){      
      if(i == cpu_id)
        continue;
      if(i < cpu_id){   //保证锁获取的顺序相同，避免死锁
        release(&kmem.lock[cpu_id]);
        acquire(&kmem.lock[i]);
        acquire(&kmem.lock[cpu_id]);
      }else{
        acquire(&kmem.lock[i]);
      }
      if(kmem.freelist[i]){                            //如果当前cpu的freelist没有空闲页面，把其他cpu的freelist抢过来 
        kmem.freelist[cpu_id] = kmem.freelist[i];
        kmem.freelist[i] = 0; 
        if(i < cpu_id){
          release(&kmem.lock[cpu_id]);
          release(&kmem.lock[i]);
          acquire(&kmem.lock[cpu_id]);
        }else{
          release(&kmem.lock[i]);
        }
        break;
      }
      if(i < cpu_id){
        release(&kmem.lock[cpu_id]);
        release(&kmem.lock[i]);
        acquire(&kmem.lock[cpu_id]);
      }else{
        release(&kmem.lock[i]);
      }
    }
    r = kmem.freelist[cpu_id];
    if(r)
      kmem.freelist[cpu_id] = r->next;
  }
  release(&kmem.lock[cpu_id]);
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
