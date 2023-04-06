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
  struct buf *b;

  for(int i=0; i < NBUCKET; i++){   //修改5：初始化每个bucket的锁
    initlock(&bcache.lock[i], "bcache");
  }

  // Create linked list of buffers
  for(int i=0; i < NBUCKET; i++){   //修改6：初始化每个bucket的头指针
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){    //修改7：初始时将每个buf都归入bucket 0中
    b->blockno = 0;
    b->ticks = 0;
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
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
    if(b->dev == dev && b->blockno == blockno){   //修改8：从当前bucket中查找已经cached的buf
      b->refcnt++;
      release(&bcache.lock[bucket_no]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  uint min_ticks = ~0;
  uint target_bucketno = 0;
  struct buf *target_buf;
  target_buf = 0;

  for(int i=0; i < NBUCKET; i++){   //修改9：搜索所有bucket，找出空闲的、lru的buf
    if(i == bucket_no){   //调整获取锁的顺序，防止死锁
      //不需要针对锁做操作
    }else if(i < bucket_no){
      release(&bcache.lock[bucket_no]);
      acquire(&bcache.lock[i]);
      acquire(&bcache.lock[bucket_no]);
    }else{
      acquire(&bcache.lock[i]);
    }
    for(b = bcache.head[i].prev; b != &bcache.head[i]; b = b->prev){
      if(b->refcnt == 0 && b->ticks <= min_ticks){
        target_buf = b;
        min_ticks = b->ticks;
        target_bucketno = i;
      }
    } 
    if(i == bucket_no){   //调整释放锁的顺序，防止死锁
      //不需要对锁做操作
    }else if(i < bucket_no){
      release(&bcache.lock[bucket_no]);
      release(&bcache.lock[i]);
      acquire(&bcache.lock[bucket_no]);
    }else{
      release(&bcache.lock[i]); 
    }
  }

  if((uint64)target_buf == 0)
    panic("bget: no buffers");

  if(target_bucketno == bucket_no){
    target_buf->dev = dev;
    target_buf->blockno = blockno;
    target_buf->valid = 0;
    target_buf->refcnt = 1;
    release(&bcache.lock[bucket_no]);
    acquiresleep(&target_buf->lock);
    return target_buf;
  }

  if(target_bucketno < bucket_no){
    release(&bcache.lock[bucket_no]);
    acquire(&bcache.lock[target_bucketno]);
    acquire(&bcache.lock[bucket_no]);
  }else{
    acquire(&bcache.lock[target_bucketno]);
  }

  target_buf->prev->next = target_buf->next;    //将target_buf从原来的bucket里面摘出
  target_buf->next->prev = target_buf->prev;
  target_buf->next = bcache.head[bucket_no].next;    //将target_buf插入当前bucket
  target_buf->prev = &bcache.head[bucket_no];
  bcache.head[bucket_no].next->prev = target_buf;
  bcache.head[bucket_no].next = target_buf;
  
  target_buf->dev = dev;
  target_buf->blockno = blockno;
  target_buf->valid = 0;
  target_buf->refcnt = 1;

  if(target_bucketno < bucket_no){
    release(&bcache.lock[bucket_no]);
    release(&bcache.lock[target_bucketno]);
  }else{
    release(&bcache.lock[target_bucketno]); 
    release(&bcache.lock[bucket_no]);
  }

  acquiresleep(&target_buf->lock);
  return target_buf;
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


  b->refcnt--;
  releasesleep(&b->lock);

  if (b->refcnt == 0) {   //修改10：不在链表中移动buf的位置，改为记录buf的时间戳
    // no one is waiting for it.
    acquire(&tickslock);
    b->ticks = ticks;
    release(&tickslock);
  }
  
}

void
bpin(struct buf *b) {
  uint bucket_no = hash(b->blockno);
  acquire(&bcache.lock[bucket_no]);
  b->refcnt++;
  release(&bcache.lock[bucket_no]);
}

void
bunpin(struct buf *b) {
  uint bucket_no = hash(b->blockno);
  acquire(&bcache.lock[bucket_no]);
  b->refcnt--;
  release(&bcache.lock[bucket_no]);
}


