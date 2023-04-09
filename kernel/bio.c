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

#define NBUCKET 13    //修改2：定义bucket数量

struct {
  struct spinlock lock[NBUCKET];    //修改3：NBUCKET个头指针，每个bucket一个锁
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];
} bcache;

uint    //修改4：定义一个哈希函数 
hash(uint blockno)
{
  return blockno % NBUCKET;
}

void
binit(void)
{
  //struct buf *b;

  for(int i=0; i < NBUCKET; i++){   //修改5：初始化每个bucket的锁
    initlock(&bcache.lock[i], "bcache.bucket");
  }

  // Create linked list of buffers
  for(int i=0; i < NBUCKET; i++){   //修改6：初始化每个bucket的头指针
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for(int i=0; i < NBUF; i++){
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
  //printf("------- bget, blockno = %d\n", blockno);

  uint bucket_no = hash(blockno);
  acquire(&bcache.lock[bucket_no]);

  // Is the block already cached?
  for(b = bcache.head[bucket_no].next; b != &bcache.head[bucket_no]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){   //修改8：从当前bucket中查找已经cached的buf
      acquire(&b->refcnt_lock);
      b->refcnt++;
      release(&b->refcnt_lock);
      //printf("release 1\n");
      release(&bcache.lock[bucket_no]);
      acquiresleep(&b->lock);
      //printf("----end bget, blockno = %d\n", blockno);
      return b;
    }
  }
  struct buf *fb = 0;
  uint min_ticks = ~0;
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

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  uint fb_bucketno ;

  for(int i=0; i < bucket_no; i++){
    acquire(&bcache.lock[i]);
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
    //printf("----end bget, blockno = %d\n", blockno);
    return fb;
  }
  
  for(int i=bucket_no+1; i < NBUCKET; i++){
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
  //printf("----end bget, blockno = %d\n", blockno);
  return fb;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  //printf("------- bread, blockno = %d\n", blockno);
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
  //printf("------- bwrite, blockno = %d\n", b->blockno);
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  //printf("------- brelse, blockno = %d\n", b->blockno);
  if(!holdingsleep(&b->lock))
    panic("brelse");

  //printf("------- brelse, blockno = %d\n", b->blockno);
  releasesleep(&b->lock);
  //printf("release b->lock, blockno=%d\n", b->blockno);

  //uint bucket_no;
  //uint blockno;   //brelse被调用说明refcnt不为0，因此这个buf不会被bget替换，不用担心这个字段被中途改变
  //blockno = b->blockno;
  //bucket_no = hash(blockno);

  //acquire(&bcache.lock[bucket_no]);
  acquire(&b->refcnt_lock);

  b->refcnt--;

  release(&b->refcnt_lock);
  //release(&bcache.lock[bucket_no]);
  return;
}

void
bpin(struct buf *b) {
  //printf("------- bpin, blockno = %d\n", b->blockno);
  //uint blockno;
  //uint bucket_no;   //上层的log层对bpin和bunpin的调用总会出现在brelse之前，这里不会有race

  //blockno = b->blockno;
  //bucket_no = hash(blockno);

  //acquire(&bcache.lock[bucket_no]);
  acquire(&b->refcnt_lock);
  b->refcnt++;
  release(&b->refcnt_lock);
  //release(&bcache.lock[bucket_no]);
}

void
bunpin(struct buf *b) {
  //printf("------- bunpin, blockno = %d\n", b->blockno);
  //uint blockno;
  //uint bucket_no;

  //blockno = b->blockno;
  //bucket_no = hash(blockno);

  //acquire(&bcache.lock[bucket_no]);
  acquire(&b->refcnt_lock);
  b->refcnt--;
  release(&b->refcnt_lock);
  //release(&bcache.lock[bucket_no]);
}


