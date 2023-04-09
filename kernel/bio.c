// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13    //bcache-修改2：定义bucket数量

struct {
  struct spinlock lock[NBUCKET];    //bcache-修改3：NBUCKET个头指针，每个bucket一个锁
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];
} bcache;

uint    //bcache-修改4：定义一个哈希函数 
hash(uint blockno)
{
  return blockno % NBUCKET;
}

void
binit(void)
{

  for(int i=0; i < NBUCKET; i++){   //bcache-修改5：初始化每个bucket的锁
    initlock(&bcache.lock[i], "bcache.bucket");
  }

  // Create linked list of buffers
  for(int i=0; i < NBUCKET; i++){   //bcache-修改6：初始化每个bucket的头指针
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for(int i=0; i < NBUF; i++){    //bcache-修改7：初始化所有buf，并尽可能均匀地分配到各个bucket中
    bcache.buf[i].blockno = 0;
    bcache.buf[i].valid = 0;
    bcache.buf[i].ticks = ~0;
    bcache.buf[i].refcnt = 0;
    bcache.buf[i].next = bcache.head[i % NBUCKET].next;
    bcache.buf[i].prev = &bcache.head[i % NBUCKET];
    initsleeplock(&bcache.buf[i].lock, "buffer");
    initlock(&bcache.buf[i].refcnt_lock, "refcnt");
    bcache.head[i % NBUCKET].next->prev = &bcache.buf[i];
    bcache.head[i % NBUCKET].next = &bcache.buf[i];
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint bucket_no = hash(blockno);
  acquire(&bcache.lock[bucket_no]);

  // Is the block already cached?
  for(b = bcache.head[bucket_no].next; b != &bcache.head[bucket_no]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){   //bcache-修改8：从当前bucket中查找已经cached的buf
      acquire(&b->refcnt_lock);
      b->refcnt++;
      release(&b->refcnt_lock);
      release(&bcache.lock[bucket_no]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf *fb = 0;
  uint min_ticks = ~0;      //bcache-修改9：查找当前bucket中LRU的空闲buffer
  for(b = bcache.head[bucket_no].next; b != &bcache.head[bucket_no]; b = b->next){
    acquire(&b->refcnt_lock);
    if(b->refcnt == 0 && b->ticks <= min_ticks){
      if(fb){
        release(&fb->refcnt_lock);
      }
      fb = b;
      min_ticks = b->ticks;
    }else{
      release(&b->refcnt_lock);
    }
  }
  if(fb){
    fb->dev = dev;
    fb->blockno = blockno;
    fb->valid = 0;
    fb->refcnt = 1;
    release(&fb->refcnt_lock);
    release(&bcache.lock[bucket_no]);
    acquiresleep(&fb->lock);
    return fb;
  }

  release(&bcache.lock[bucket_no]);

  uint fb_bucketno ;

  for(int i=0; i < bucket_no; i++){   //bcache-修改10：尝试在编号小于当前bucket的bucket中查找空闲buffer
    acquire(&bcache.lock[i]);         //查找的都是某个bucket内局部LRU的buffer
    acquire(&bcache.lock[bucket_no]);
    for(b = bcache.head[i].next; b != &bcache.head[i]; b = b->next){
      acquire(&b->refcnt_lock);
      if(b->refcnt == 0 && b->ticks <= min_ticks){
        if(fb){
          release(&fb->refcnt_lock);
        }
        fb = b;
        min_ticks = b->ticks;
      }else{
        release(&b->refcnt_lock);
      }
    }
    if(fb){
      fb_bucketno = i;
      break;
    }
    release(&bcache.lock[bucket_no]);
    release(&bcache.lock[i]);
  }

  if(fb){
    fb->next->prev = fb->prev;
    fb->prev->next = fb->next;

    fb->next = bcache.head[bucket_no].next;
    fb->prev = &bcache.head[bucket_no];

    bcache.head[bucket_no].next->prev = fb;
    bcache.head[bucket_no].next = fb;

    fb->dev = dev;
    fb->blockno = blockno;
    fb->valid = 0;
    fb->refcnt = 1;
  
    release(&fb->refcnt_lock);
    release(&bcache.lock[fb_bucketno]);
    release(&bcache.lock[bucket_no]);
    acquiresleep(&fb->lock);
    return fb;
  }
  
  for(int i=bucket_no+1; i < NBUCKET; i++){   //bcache-修改11：尝试在大于当前bucket编号的bucket中寻找空闲buffer
    acquire(&bcache.lock[bucket_no]);
    acquire(&bcache.lock[i]);
    for(b = bcache.head[i].next; b != &bcache.head[i]; b = b->next){
      acquire(&b->refcnt_lock);
      if(b->refcnt == 0 && b->ticks <= min_ticks){
        if(fb){
          release(&fb->refcnt_lock);
        }
        fb = b;
        min_ticks = b->ticks;
      }else{
        release(&b->refcnt_lock);
      }
    }
    if(fb){
      fb_bucketno = i;
      break;
    }
    release(&bcache.lock[i]);
    release(&bcache.lock[bucket_no]);
  }

  if(fb == 0)
    panic("bget: no buffers");

  fb->next->prev = fb->prev;
  fb->prev->next = fb->next;

  fb->next = bcache.head[bucket_no].next;
  fb->prev = &bcache.head[bucket_no];

  bcache.head[bucket_no].next->prev = fb;
  bcache.head[bucket_no].next = fb;

  fb->dev = dev;
  fb->blockno = blockno;
  fb->valid = 0;
  fb->refcnt = 1;
  
  release(&fb->refcnt_lock);
  release(&bcache.lock[fb_bucketno]);
  release(&bcache.lock[bucket_no]);
  acquiresleep(&fb->lock);
  return fb;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&b->refcnt_lock);   //bcache-修改12：此时buf->refcnt_lock才是保护refcnt的锁

  b->refcnt--;

  release(&b->refcnt_lock);
  return;
}

void
bpin(struct buf *b) {
  acquire(&b->refcnt_lock);   //bcache-修改13：同brelse
  b->refcnt++;
  release(&b->refcnt_lock);
}

void
bunpin(struct buf *b) {
  acquire(&b->refcnt_lock);   //bcache-修改14：同brelse
  b->refcnt--;
  release(&b->refcnt_lock);
}


