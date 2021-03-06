#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define INDIRECT_BLOCK_SIZE 128
#define DIRECT_BLOCKS 12
#define INDIRECT_BLOCKS 140
#define DOUBLY_BLOCKS 16524

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    //block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[110];               /* Not used. */

    /* Added for extensible files */
    block_sector_t direct[14];
    size_t blocks;

    /* Added for directories. */
    bool is_dir;
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    
    struct lock inode_lock;
  };


void inode_free (struct inode *inode);
off_t inode_grow (struct inode_disk *id, off_t length);


/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
  {
    size_t index = pos / BLOCK_SECTOR_SIZE;
    block_sector_t buffer[INDIRECT_BLOCK_SIZE];

    /* Handle Direct Blocks. */
    if (index < DIRECT_BLOCKS) {
      return inode->data.direct[index];
    }

    /* Handle Indirect Blocks. */
    else if (index < INDIRECT_BLOCKS) {
      index -= DIRECT_BLOCKS;
      cache_read (inode->data.direct[12], &buffer);
      return buffer[index];
    }

    /* Handle Doubly-Indirect Blocks. */
    else if (index < DOUBLY_BLOCKS) {
      /* First Level. */
      cache_read (inode->data.direct[13], &buffer);
      index -= INDIRECT_BLOCKS;

      /* Second Level. */
      cache_read (buffer[index / INDIRECT_BLOCK_SIZE], &buffer);
      return buffer[index % INDIRECT_BLOCK_SIZE];
    }
  }
  return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = 0;
      disk_inode->blocks = 0;
      disk_inode->is_dir = is_dir;
      disk_inode->magic = INODE_MAGIC;

      disk_inode->length = inode_grow (disk_inode, length);
      ASSERT (disk_inode->length == length);

      cache_write (sector, disk_inode);
      success = true;
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->inode_lock);

  cache_read (inode->sector, &inode->data);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  { 
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
    {
      free_map_release (inode->sector, 1);
      inode_free (inode);
    }
    else
    {
      cache_write (inode->sector, &inode->data);
    }
    free (inode); 
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (offset + size > inode_length (inode)) {
    if (!inode->data.is_dir)
      lock_acquire (&inode->inode_lock);

    inode->data.length = inode_grow (&inode->data, offset + size);
    cache_write (inode->sector, & inode->data);
  
    if (!inode->data.is_dir)
      lock_release (&inode->inode_lock);
  }


  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

off_t
inode_grow (struct inode_disk *id, off_t length)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t new_sectors = bytes_to_sectors (length) - bytes_to_sectors (id->length);

  if (new_sectors == 0)
    return length;

  /* Handle direct blocks. */
  while (id->blocks < DIRECT_BLOCKS)
  {
    free_map_allocate (1, &id->direct[id->blocks]);
    cache_write (id->direct[id->blocks], zeros);
    new_sectors--;
    id->blocks++;
    
    if (new_sectors == 0) 
      return length;
  }

  /* Handle indirect blocks. */
  block_sector_t buffer[INDIRECT_BLOCK_SIZE];

  if (id->blocks == DIRECT_BLOCKS)
    free_map_allocate (1, &id->direct[DIRECT_BLOCKS]);
  else
    cache_read (id->direct[DIRECT_BLOCKS], &buffer);

  while (id->blocks < INDIRECT_BLOCKS)
  {
    free_map_allocate (1, &buffer[id->blocks - DIRECT_BLOCKS]);
    cache_write (buffer[id->blocks - DIRECT_BLOCKS], zeros);
    new_sectors--;
    id->blocks++;
    
    if (new_sectors == 0) 
      break;
  }
  cache_write (id->direct[DIRECT_BLOCKS], &buffer);
  if (new_sectors == 0)
    return length;

  /* Handle doubly indirect blocks. */
  block_sector_t buffer1[INDIRECT_BLOCK_SIZE], buffer2[INDIRECT_BLOCK_SIZE];

  if (id->blocks == INDIRECT_BLOCKS)
    free_map_allocate (1, &id->direct[DIRECT_BLOCKS + 1]);
  else
    cache_read (id->direct[DIRECT_BLOCKS + 1], &buffer1);

  size_t index1 = (id->blocks - INDIRECT_BLOCKS) / INDIRECT_BLOCK_SIZE;
  size_t index2 = (id->blocks - INDIRECT_BLOCKS) % INDIRECT_BLOCK_SIZE;

  while (index1 < INDIRECT_BLOCK_SIZE)
  {
    if (index2 == 0)
      free_map_allocate (1, &buffer1[index1]);
    else
      cache_read (buffer1[index1], &buffer2);

    while (index2 < INDIRECT_BLOCK_SIZE)
    {
      free_map_allocate (1, &buffer2[index2]);
      cache_write (buffer2[index2], zeros);
      id->blocks++;
      index2++;
      new_sectors--;

      if (new_sectors == 0)
        break;
    }

    /* Write to level 1 buffer. */
    cache_write (buffer1[index1], &buffer2);

    index1++;
    index2 = 0;

    if (new_sectors == 0)
      break;
  }
  cache_write (id->direct[DIRECT_BLOCKS + 1], &buffer1);
  return length;
  
}

void
inode_free (struct inode *inode)
{
  struct inode_disk *id = &inode->data;
  size_t sectors = bytes_to_sectors (id->length);
  size_t index = 0;
  if (sectors == 0)
    return;

  /* Direct Blocks. */
  while (index < DIRECT_BLOCKS)
  {
    free_map_release (id->direct[index], 1);
    sectors--;
    index++;
    if (sectors == 0)
      return;
  }

  /* Indirect Blocks. */
  block_sector_t buffer[INDIRECT_BLOCK_SIZE];
  size_t i = 0;
  cache_read (id->direct[index], &buffer);
  size_t to_free = sectors < INDIRECT_BLOCK_SIZE ? sectors : INDIRECT_BLOCK_SIZE;

  for (i = 0; i < to_free; i++)
  {
    free_map_release (buffer[i], 1);
    sectors--;
  }
  free_map_release (id->direct[index], 1);
  index++;
  if (sectors == 0)
    return;

  /* Doubly-Indirect Blocks. */
  size_t j = 0;
  i = 0;
  block_sector_t buffer1[INDIRECT_BLOCK_SIZE], buffer2[INDIRECT_BLOCK_SIZE];
  cache_read (id->direct[index], &buffer1);

  size_t doubly_blocks = DIV_ROUND_UP (sectors, INDIRECT_BLOCK_SIZE);
  for (i = 0; i < doubly_blocks; i++)
  {
    size_t to_free = sectors < INDIRECT_BLOCK_SIZE ? sectors : INDIRECT_BLOCK_SIZE;
    cache_read (buffer1[i], &buffer2);
    for (j = 0; j < to_free; j++)
    {
      free_map_release (buffer2[j], 1);
      sectors--;
    }
    free_map_release (buffer1[i], 1);
  }
  free_map_release (id->direct[index], 1);
}

int inode_get_open_cnt (const struct inode *inode) {
  return inode->open_cnt;
}

bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir;
}

bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}