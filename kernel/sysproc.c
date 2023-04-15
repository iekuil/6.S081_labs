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
  //printf("pid %d calling mmap\n", p->pid);
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
  int map_private = flags & MAP_PRIVATE;

  //输入了没有意义的权限bit
  if(!prot_read && !prot_write && !prot_exec){
    return -1;
  }

  //确认映射的权限没有超出此前open()文件的时候设置的权限
  struct file *fp = p->ofile[fd];
  if((!fp->readable && prot_read) || (!map_private && (!fp->writable && prot_write))){
    return -1;
  }

  //判断即将映射的空间会不会溢出到内存中的堆部分
  map_size = PGROUNDUP(length);
  if(p->highest_unused - map_size < PGROUNDUP(p->sz)){
    return -1;
  }

  //查找空闲的vma表项
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
  p->vma[vma_no].filep = (uint64)p->ofile[fd];
  p->vma[vma_no].start_vp = p->highest_unused - map_size;
  p->vma[vma_no].map_size = map_size;
  p->vma[vma_no].prot = prot;
  p->vma[vma_no].file_length = length;
  p->vma[vma_no].flags = flags;
  p->vma[vma_no].offset = offset;

  

  //这里会先将相应的虚拟地址在页表上映射到0，并标记为用户不可见
  //之后在trap处理的时候会unmap，再申请物理页面、mappage
  //这么干主要是防止有的页还没被懒拷贝到内存里，就unmap了，从而在unmap的时候会由于不存在相应的页表项而panic
  //uint64 start_addr = p->vma[vma_no].start_vp;
  //uint64 end_addr = p->vma[vma_no].start_vp + map_size;
  //for(; start_addr < end_addr; start_addr += PGSIZE ){
  //  if(mappages(p->pagetable, start_addr, PGSIZE, 0, PTE_MMAP) < 0){
  //    return -1;
  //  }
  //}
  //if(mappages(p->pagetable, p->vma[vma_no].start_vp, p->vma[vma_no].map_size, KERNBASE, PTE_MMAP) < 0){
  //  return -1;
  //}
  filedup(p->ofile[fd]);
  p->highest_unused -= map_size;
  
  //printf("\n ---pid %d mmap: start_vp=%p, mapsize=%p\n", p->pid, p->vma[vma_no].start_vp, map_size);
  //vmprint(p->pagetable, 3);
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
  size_t length;
  uint64 pgrd_addr;
  size_t unmap_size;
  if((argaddr(0, &addr) < 0) || (argaddr(1, &length) < 0)  ){
    return -1;
  }


  //对于给定的unmap地址和大小进行按页对齐
  pgrd_addr = PGROUNDDOWN(addr);
  unmap_size = PGROUNDUP(length);

  struct  proc *p = myproc();
  //printf("pid %d calling munmap\n", p->pid);

  int     flags;
  off_t   offset = 0;

  uint64  start_vp;
  int     map_size;

  int mapped_flag = 0;

  //粗略判断给定的addr是否在mmap已映射的地址范围
  if(pgrd_addr < p->highest_unused){
    return -1;
  }

  //逐一比对vma中每个表项的映射范围
  int vma_no;
  for(vma_no = 0; vma_no < MAX_VMA; vma_no++){
    start_vp = p->vma[vma_no].start_vp;
    map_size = p->vma[vma_no].map_size;
    if(pgrd_addr >= start_vp && pgrd_addr <= start_vp + map_size){
      mapped_flag = 1;
      break;
    }
  }
  if(mapped_flag == 0){
    return -1;
  }

 // printf("to unmap: addr %p, length %p. vma_no %d, vma start-vp %p, vma map-size %p\n", addr, length, vma_no, start_vp, map_size);
  //printf("\n---in unmap: chekcking pagetable\n");
  //vmprint(p->pagetable, 3);
  //进一步判断unmap的地址是否符合要求：
  //  位于映射范围的开头或结尾，
  //  不能把原来的映射范围分割成两半

  int unmap_at_head = 0;
  int unmap_at_tail = 0;

  if(pgrd_addr == start_vp){
    unmap_at_head = 1;
  }
  if((pgrd_addr + unmap_size) == (start_vp + map_size)){
    unmap_at_tail = 1;
  }

  if(!unmap_at_head && !unmap_at_tail){
    return -1;
  }

  //当需要写回时，计算文件中的偏移和长度
  flags = p->vma[vma_no].flags;
  offset = p->vma[vma_no].offset;

  int write_back = flags & MAP_SHARED;
  int wb_offset;

  if(write_back){
    wb_offset = offset + addr - start_vp;
    struct file *f = (struct file *)p->vma[vma_no].filep;

    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;

    int i = 0;
    int r = 0;

    while(i < length){    
      int n1 = length - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, wb_offset, n1)) > 0)
        wb_offset += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
  }

  int npages = unmap_size / PGSIZE;
  //printf("\n---pid %d munmap, before. addr=%p, size=%p, num=%d\n", p->pid, pgrd_addr, unmap_size, npages);
  //vmprint(p->pagetable, 3);
  //这里似乎没能正确释放掉fork后进程的页面(×不是这里)
  for(int i = 0; i < npages; i++){
    if(walkaddr(p->pagetable, pgrd_addr+ i*PGSIZE) == 0){
      //uvmunmap(p->pagetable, pgrd_addr + i*PGSIZE, 1, 0);
    } else{
      uvmunmap(p->pagetable, pgrd_addr + i*PGSIZE, 1, 1);
    }
  }
  //printf("\n---pid %d munmap, after\n", p->pid);
  //vmprint(p->pagetable, 3);

  if(unmap_at_head && unmap_at_tail){
    p->vma[vma_no].used = 0;
    p->vma[vma_no].start_vp = 0;
    p->vma[vma_no].map_size = 0;
    p->vma[vma_no].prot = 0;
    p->vma[vma_no].file_length = 0;
    p->vma[vma_no].flags = 0;
    p->vma[vma_no].offset = 0;

    fileclose((struct file *)p->vma[vma_no].filep);
    p->vma[vma_no].filep = 0;

    //printf("\n ----pid %d unmap\n", p->pid);
    //vmprint(p->pagetable, 3);
    
    return 0;
  } else if (unmap_at_head){
    p->vma[vma_no].start_vp = addr + unmap_size;
    p->vma[vma_no].offset += unmap_size;
    p->vma[vma_no].map_size -= unmap_size;

    //printf("\n pid %d in munmap, updated vma %d, start_vp=%p, size=%p\n", p->pid, vma_no, p->vma[vma_no].start_vp, p->vma[vma_no].map_size);
    return 0;
  } else{
    p->vma[vma_no].map_size -= unmap_size;
    return 0;
  }
  return 0;
}
