#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

//mmap: 需要里面的宏定义
#include "fcntl.h" 
#include "sleeplock.h" 
#include "fs.h"  
#include "file.h" 

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


//mmap修改7：添加mmap和munmap系统调用的实现
uint64
sys_mmap(void)
{
/*
  char* mmap(void *addr, size_t length, int prot, int flags,
              int fd, off_t offset);
  0. 调用规则：八个参数以内都是使用寄存器传参
  1. 进程的内存布局：
    [user text and data | stack guard page(防止栈溢出) | 栈(固定大小一个page) | 已分配heap | 未分配heap | trapframe | trampoline]
  2. proc->sz 记录了当前进程p的内存大小(从0开始，到已分配heap的末端)
        ->需要在VMA中记录相应的映射区域，以供之后释放
  3. 将pte中的保留位用来标记mmap映射页面
  4. hints中提到懒拷贝不应该分配物理页面，
        ->不能用uvmalloc和uvmdealloc
  5. 为了实现懒拷贝，mmap要做的工作：
        (1)寻找空闲的连续页表项，标记为已使用，但是不分配物理页面也不建立映射；-> valid位设1，user位设0，物理页号设0。可以利用walk()函数，从高地址向低查找
        (2)在VMA中记录虚拟地址空间的起点和终点；
*/
  struct  proc *p = myproc();
  //void    *addr = 0;
  size_t  length;
  int     prot;
  int     flags;
  int     fd;
  off_t   offset = 0;
  
  int     map_size;

  if((argaddr(1, &length) < 0) || (argint(2, &prot) < 0) || (argint(3, &flags) < 0) ||
      (argint(4, &fd) < 0) ){
    return -1;
  }

  int prot_read = prot & PROT_READ;
  int prot_write = prot & PROT_WRITE;
  int prot_exec = prot & PROT_EXEC;

  if(!prot_read && !prot_write && !prot_exec){
    return -1;
  }

  map_size = PGROUNDUP(length);
  if(p->highest_unused - map_size < PGROUNDUP(p->sz)){
    return -1;
  }

  int vma_no;
  for(vma_no = 0; vma_no < MAX_VMA; vma_no++)
  {
    if(p->vma[vma_no].used == 0)
      break;
  }
  if(vma_no == MAX_VMA && p->vma[vma_no].used != 0){
    return -1;
  }

  p->vma[vma_no].used = 1;
  p->vma[vma_no].fd = fd;
  p->vma[vma_no].start_vp = p->highest_unused - map_size + PGSIZE;
  p->vma[vma_no].map_size = map_size;
  p->vma[vma_no].prot = prot;
  p->vma[vma_no].file_length = length;
  p->vma[vma_no].flags = flags;
  p->vma[vma_no].offset = offset;

  filedup(p->ofile[fd]);
  p->highest_unused -= map_size;

  return p->vma[vma_no].start_vp;
}

uint64
sys_munmap(void)
{
  /*
    int munmap(void *addr, size_t length);
    1. 在释放proc时，freeproc()->proc_freepagetable()->uvmfree()
        因此此时释放物理页是从0开始，直到heap的末端
          -> 需要额外调用munmap释放掉由mmap映射的页面 
    2.  当需要unmap部分片段时，需要释放掉相应的物理页和页表项，并修改vma中的offset和length
  */
  uint64 addr;
  size_t unmap_size;
  if((argaddr(0, &addr) < 0) || (argaddr(2, &unmap_size) < 0)  ){
    return -1;
  }

  //对于给定的unmap地址和大小进行按页对齐
  addr = PGROUNDDOWN(addr);
  unmap_size = PGROUNDUP(unmap_size);

  struct  proc *p = myproc();

  //size_t  length;
  int     flags;
  int     fd;
  off_t   offset = 0;

  uint64  start_vp;
  int     map_size;

  int mapped_flag = 0;

  //粗略判断给定的addr是否在mmap已映射的地址范围
  if(addr <= p->highest_unused){
    return -1;
  }

  //逐一比对vma中每个表项的映射范围
  int vma_no;
  for(vma_no = 0; vma_no < MAX_VMA; vma_no++){
    start_vp = p->vma[vma_no].start_vp;
    map_size = p->vma[vma_no].map_size;
    if(addr >= start_vp && addr <= start_vp + map_size){
      mapped_flag = 1;
      break;
    }
  }
  if(mapped_flag == 0){
    return -1;
  }

  //进一步判断unmap的地址是否符合要求：
  //  位于映射范围的开头或结尾，
  //  不能把原来的映射范围分割成两半

  int unmap_at_head = 0;
  int unmap_at_tail = 0;

  if(addr == start_vp){
    unmap_at_head = 1;
  }
  if((addr + unmap_size) == (start_vp + map_size)){
    unmap_at_tail = 1;
  }

  if(!unmap_at_head && !unmap_at_tail){
    return -1;
  }

  //当需要写回时，计算文件中的偏移和长度
  fd = p->vma[vma_no].fd;
  flags = p->vma[vma_no].flags;
  offset = p->vma[vma_no].offset;

  int write_back = flags & MAP_SHARED;
  int wb_offset;
  //int wb_size;

  if(write_back){
    wb_offset = offset + addr - start_vp;
    int fd = p->vma[vma_no].fd;
    struct file *f = p->ofile[fd];
    struct inode *ip = f->ip;

    ilock(ip);
    writei(ip, 1, addr, wb_offset, unmap_size);
    iunlock(ip);
  }

  uvmunmap(p->pagetable, addr, unmap_size/PGSIZE, 1);

  if(unmap_at_head && unmap_at_tail){
    p->vma[vma_no].used = 0;
    p->vma[vma_no].start_vp = 0;
    p->vma[vma_no].map_size = 0;
    p->vma[vma_no].prot = 0;
    p->vma[vma_no].file_length = 0;
    p->vma[vma_no].flags = 0;
    p->vma[vma_no].offset = 0;

    fileclose(p->ofile[fd]);
    return 0;
  } else if (unmap_at_head){
    p->vma[vma_no].start_vp = addr + unmap_size;
    return 0;
  } else{
    p->vma[vma_no].map_size -= unmap_size;
    return 0;
  }
  return 0;
}
