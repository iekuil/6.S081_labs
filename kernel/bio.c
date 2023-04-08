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
  struct spinlock bb_lock[NBUCKET];    //修改3：NBUCKET个头指针，每个bucket一个锁
  struct spinlock fb_lock[NBUCKET];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf bb_head[NBUCKET];
  struct buf fb_head[NBUCKET];
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
    initlock(&bcache.bb_lock[i], "bcache_bb");
    initlock(&bcache.fb_lock[i], "bcache_fb");
  }

  // Create linked list of buffers
  for(int i=0; i < NBUCKET; i++){   //修改6：初始化每个bucket的头指针
    bcache.bb_head[i].prev = &bcache.bb_head[i];
    bcache.bb_head[i].next = &bcache.bb_head[i];

    bcache.fb_head[i].prev = &bcache.fb_head[i];
    bcache.fb_head[i].next = &bcache.fb_head[i];
  }
/*
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){    //修改7：初始时将每个buf都归入bucket 0中
    b->blockno = 0;
    b->ticks = 0;
    b->next = bcache.fb_head[0].next;
    b->prev = &bcache.fb_head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.fb_head[0].next->prev = b;
    bcache.fb_head[0].next = b;
  }
*/
  for(int i=0; i < NBUF; i++){
    bcache.buf[i].blockno = i % NBUCKET;
    bcache.buf[i].ticks = 0;
    bcache.buf[i].next = bcache.fb_head[i % NBUCKET].next;
    bcache.buf[i].prev = &bcache.fb_head[i % NBUCKET];
    initsleeplock(&bcache.buf[i].lock, "buffer");
    bcache.fb_head[i % NBUCKET].next->prev = &bcache.buf[i];
    bcache.fb_head[i % NBUCKET].next = &bcache.buf[i];
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
  acquire(&bcache.bb_lock[bucket_no]);

  // Is the block already cached?
  for(b = bcache.bb_head[bucket_no].next; b != &bcache.bb_head[bucket_no]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){   //修改8：从当前bucket中查找已经cached的buf
      b->refcnt++;
      release(&bcache.bb_lock[bucket_no]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  acquire(&bcache.fb_lock[bucket_no]);
  for(b = bcache.fb_head[bucket_no].next; b != &bcache.fb_head[bucket_no]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      b->next->prev = b->prev;
      b->prev->next = b->next;

      b->next = bcache.bb_head[bucket_no].next;
      b->prev = &bcache.bb_head[bucket_no];

      bcache.bb_head[bucket_no].next->prev = b;
      bcache.bb_head[bucket_no].next = b;

      release(&bcache.fb_lock[bucket_no]);
      release(&bcache.bb_lock[bucket_no]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.fb_lock[bucket_no]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  uint fb_blockno = 0;
  uint min_ticks = ~0;
  uint fb_bucketno = ~0;
  struct buf *fb;
  fb = 0;
  
  for(int i=0; i < NBUCKET; i++){
    acquire(&bcache.fb_lock[i]);
    for(b = bcache.fb_head[i].next; b != &bcache.fb_head[i]; b = b->next){
      if(b->refcnt == 0 && b->ticks <= min_ticks){
        if(fb != 0 && i != fb_bucketno){
          release(&bcache.fb_lock[fb_bucketno]);
        }
        fb_blockno = b->blockno;
        fb_bucketno = hash(fb_blockno);
        min_ticks = b->ticks;
        fb = b;
      }
    }
    if(fb_bucketno != i){
      release(&bcache.fb_lock[i]);
    }
  }
  if(fb == 0)
    panic("bget: no buffers");
  fb->next->prev = fb->prev;
  fb->prev->next = fb->next;

  fb->next = bcache.bb_head[bucket_no].next;
  fb->prev = &bcache.bb_head[bucket_no];

  bcache.bb_head[bucket_no].next->prev = fb;
  bcache.bb_head[bucket_no].next = fb;

  fb->dev = dev;
  fb->blockno = blockno;
  fb->valid = 0;
  fb->refcnt = 1;
  
  release(&bcache.fb_lock[fb_bucketno]);
  release(&bcache.bb_lock[bucket_no]);
  acquiresleep(&fb->lock);
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

  uint bucket_no;
  uint blockno;
  while(1)
  {
    blockno = b->blockno;
    bucket_no = hash(blockno);
    acquire(&bcache.bb_lock[bucket_no]);
    if(blockno != b->blockno){
      panic("brelse: changed");
      release(&bcache.bb_lock[bucket_no]);
    }else{
      break;
    }
  }

  b->refcnt--;

  if (b->refcnt == 0) {   //修改10：不在链表中移动buf的位置，改为记录buf的时间戳
    // no one is waiting for it.
    acquire(&tickslock);
    b->ticks = ticks;
    release(&tickslock);

    acquire(&bcache.fb_lock[bucket_no]);

    b->next->prev = b->prev;
    b->prev->next = b->next;

    b->next = bcache.fb_head[bucket_no].next;
    b->prev = &bcache.fb_head[bucket_no];

    bcache.fb_head[bucket_no].next->prev = b;
    bcache.fb_head[bucket_no].next = b;

    release(&bcache.fb_lock[bucket_no]);
    
  }
  release(&bcache.bb_lock[bucket_no]);
  
  return;
}

void
bpin(struct buf *b) {
  //printf("------- bpin, blockno = %d\n", b->blockno);
  uint blockno;
  uint bucket_no;

  while(1){
    blockno = b->blockno;
    bucket_no = hash(blockno);

    acquire(&bcache.bb_lock[bucket_no]);
    acquire(&bcache.fb_lock[bucket_no]);
    if(blockno != b->blockno){
      panic("bpin");
      release(&bcache.fb_lock[bucket_no]);
      release(&bcache.bb_lock[bucket_no]);
    }else{
      break;
    }
  }
  b->refcnt++;
  release(&bcache.fb_lock[bucket_no]);
  release(&bcache.bb_lock[bucket_no]);
}

void
bunpin(struct buf *b) {
  //printf("------- bunpin, blockno = %d\n", b->blockno);
  uint blockno;
  uint bucket_no;

  while(1){
    blockno = b->blockno;
    bucket_no = hash(blockno);

    acquire(&bcache.bb_lock[bucket_no]);
    acquire(&bcache.fb_lock[bucket_no]);
    if(blockno != b->blockno){
      panic("bunpin");
      release(&bcache.fb_lock[bucket_no]);
      release(&bcache.bb_lock[bucket_no]);
    }else{
      break;
    }
  }
  b->refcnt--;
  release(&bcache.fb_lock[bucket_no]);
  release(&bcache.bb_lock[bucket_no]);
}


