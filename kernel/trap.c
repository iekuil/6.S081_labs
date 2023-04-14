#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

//mmap: 需要里面的宏定义
#include "fcntl.h" 
#include "sleeplock.h" 
#include "fs.h"  
#include "file.h"



struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

int
lazy_mmap(void)
{
  uint64 scause = r_scause();
  //判断是否属于page fault
  if(scause != 12 && scause != 13 && scause != 15){
    intr_on();
    return -1;
  }

  //读取造成pagefault的地址
  uint64 err_va = r_stval();
  struct proc *p = myproc();

  //粗略判断异常地址是否在mmap已分配的区域内
  if(PGROUNDDOWN(err_va) < p->highest_unused){
    intr_on();
    return -1;
  }

  //逐个比对p的每个vma的地址范围
  //进一步判断异常地址是否属于mmap的映射范围，
  //同时确定异常地址所属的vma
  uint64 start_vp;
  int map_size;
  int vma_no;
  int flag = 0;

  for(vma_no = 0; vma_no < MAX_VMA; vma_no++){
    if(p->vma[vma_no].used == 0)
      continue;
    start_vp = p->vma[vma_no].start_vp;
    map_size = p->vma[vma_no].map_size;

    if(err_va >= start_vp && err_va <= start_vp + map_size){
      flag = 1;
      break;
    }
  }

  if(flag == 0){
    intr_on();
    return -1;
  }

  //利用vma的起始地址和当前异常地址，计算进行文件读取的起始处的偏移
  off_t offset = p->vma[vma_no].offset;
  uint64 page_to_map = PGROUNDDOWN(err_va);
  offset = offset + (page_to_map - start_vp);

  //分配物理页面并设置页表项
  int prot = p->vma[vma_no].prot;
  int prot_read = prot & PROT_READ;
  int prot_write = prot & PROT_WRITE;
  int prot_exec = prot & PROT_EXEC;

  int perm = PTE_MMAP | PTE_U;
  if(prot_read)
    perm |= PTE_R;
  if(prot_write)
    perm |= PTE_W;
  if(prot_exec)
    perm |= PTE_X;

  char *pa;
  if((pa = kalloc()) == 0){
    intr_on();
    return -1;
  }
  memset(pa, 0, PGSIZE);

  if(mappages(p->pagetable, page_to_map, PGSIZE, (uint64)pa, perm) < 0){
    kfree(pa);
    intr_on();
    return -1;
  }

  //读取文件，写入相应的页面
  struct file *f ;
  f = (struct file*)p->vma[vma_no].filep;  // ↓在出现pagefault之前，这里的f指针是0

  struct inode *ip = f->ip;     //这一行的访问会出现page fault, 然后就panic: kerneltrap, why?
                                //并且经过一次mmap、munmap之后再mmap才会出现

  ilock(ip);
  if(readi(ip, 0, (uint64)pa, offset, PGSIZE) < 0){
    kfree(pa);
    iunlock(ip);
    intr_on();
    return -1;
  };
  iunlock(ip);
  intr_on();
  return 0;
}
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();  
  } else if(lazy_mmap() == 0){
    //lazy mmap
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

