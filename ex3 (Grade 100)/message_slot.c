#undef __KERNEL__
#define __KERNEL__
#undef MODULE
#define MODULE


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");

#include "message_slot.h"

//================== DATA STRUCTURES ===========================
/* hash table implementation is based on: https://benhoyt.com/writings/hash-table-in-c/ (modified for our use)
   with int hash function from: https://stackoverflow.com/questions/664014/ */

typedef struct {
  char len;
  char contents[128];
} message;

// Hash table entry (slot may be filled or empty).
typedef struct {
    uint64_t key;  // key matches the channel this entry represents (with the minor in the upper bits). 0 for empty slot
    message* value;
} ht_entry;

// Hash table structure: create with ht_create, free with ht_destroy.
typedef struct {
    ht_entry* entries;  // hash slots
    size_t capacity;    // size of _entries array
    size_t length;      // number of items in hash table
} ht;

#define INITIAL_CAPACITY 16

ht* ht_create(void) {
    // Allocate space for hash table struct.
    ht* table = kmalloc(sizeof(ht), GFP_KERNEL);
    if (table == NULL) {
        return NULL;
    }
    table->length = 0;
    table->capacity = INITIAL_CAPACITY;

    // Allocate (zero'd) space for entry buckets.
    table->entries = kmalloc(table->capacity * sizeof(ht_entry), GFP_KERNEL);
    if (table->entries == NULL) {
        kfree(table);
        return NULL;
    }
    memset(table->entries, 0, table->capacity * sizeof(ht_entry));
    return table;
}

void ht_destroy(ht* table) {
    size_t i;

    // first free all messages, then the entries buffer and finally the table itself
    for (i = 0; i < table->capacity; i++) {
        kfree((void*)table->entries[i].value);
    }
    kfree(table->entries);
    kfree(table);
}

uint64_t hash(uint64_t x) {
    x = (x ^ (x >> 30)) * U64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * U64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return x;
}

message* ht_get(ht* table, uint64_t key) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hashed = hash(key);
    size_t index = (size_t)(hashed & (uint64_t)(table->capacity - 1));

    // Loop till we find an empty entry.
    while (table->entries[index].key != 0) {
        if (key == table->entries[index].key) {
            // Found key, return value.
            return table->entries[index].value;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= table->capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }
    return NULL;
}

// Internal function to set an entry (without expanding table).
static uint64_t ht_set_entry(ht_entry* entries, size_t capacity,
        uint64_t key, message* value, size_t* plength) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hashed = hash(key);
    size_t index = (size_t)(hashed & (uint64_t)(capacity - 1));

    // Loop till we find an empty entry.
    while (entries[index].key != 0) {
        if (key == entries[index].key) {
            // Found key (it already exists), update value.
            kfree(entries[index].value); // free previous message
            entries[index].value = value;
            return entries[index].key;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }

    // Didn't find key, update length if needed, then insert it.
    if (plength != NULL) {
        (*plength)++;
    }
    entries[index].key = key;
    entries[index].value = value;
    return key;
}

// Expand hash table to twice its current size. Return true on success,
// false if out of memory.
static bool ht_expand(ht* table) {
    size_t new_capacity;
    ht_entry* new_entries;
    size_t i;

    // Allocate new entries array.
    new_capacity = table->capacity * 2;
    if (new_capacity < table->capacity) {
        return false;  // overflow (capacity would be too big)
    }
    new_entries = kmalloc(new_capacity * sizeof(ht_entry), GFP_KERNEL);
    if (new_entries == NULL) {
        return false;
    }
    memset(table->entries, 0, table->capacity * sizeof(ht_entry));

    // Iterate entries, move all non-empty ones to new table's entries.
    for (i = 0; i < table->capacity; i++) {
        ht_entry entry = table->entries[i];
        if (entry.key != 0) {
            ht_set_entry(new_entries, new_capacity, entry.key,
                         entry.value, NULL);
        }
    }

    // Free old entries array and update this table's details.
    kfree(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
    return true;
}

const uint64_t ht_set(ht* table, uint64_t key, message* value) {
    // If length will exceed half of current capacity, expand it.
    if (table->length >= table->capacity / 2) {
        if (!ht_expand(table)) {
            return 0;
        }
    }

    // Set entry and update length.
    return ht_set_entry(table->entries, table->capacity, key, value,
                        &table->length);
}

size_t ht_length(ht* table) {
    return table->length;
}

//================== GLOBAL DATA ===========================
static ht* messages;

//================== DEVICE FUNCTIONS ===========================
static int device_open( struct inode* inode,
                        struct file*  file )
{
  printk("Invoking device_open on node %u\n", iminor(inode));

  file->private_data = (void *)0;
  return SUCCESS;
}

//---------------------------------------------------------------
static int device_release( struct inode* inode,
                           struct file*  file)
{
  return SUCCESS;
}

//---------------------------------------------------------------
static ssize_t device_read( struct file* file,
                            char __user* buffer,
                            size_t       length,
                            loff_t*      offset )
{
  // minor and channel are logically 32-bit (minor even 20 bit) but define them here as 64 bit to more cleanly calculate the key
  uint64_t minor;
  uint64_t channel;
  uint64_t key;
  message* read_message;
  char * origin;

  channel = (uint64_t) file->private_data;
  if (channel == 0)
  {
    return -EINVAL;
  }

  minor = (uint64_t) iminor(file->f_inode);
  key = (minor << 32) | channel;
  read_message = ht_get(messages, key);

  if (read_message == NULL)
  {
    return -EWOULDBLOCK;
  }

  if (read_message->len > length)
  {
    return -ENOSPC;
  }

  origin = read_message->contents;

  printk("reading from minor %llu and channel %llu", minor, channel);
  printk("message length is %d, message read is: %.*s", read_message->len, read_message->len, origin);

  if (copy_to_user(buffer, origin, read_message->len) != 0)
  {
    return -EINVAL; // user provided bad buffer
  }

  return read_message->len;
}

//---------------------------------------------------------------
static ssize_t device_write( struct file*       file,
                             const char __user* buffer,
                             size_t             length,
                             loff_t*            offset)
{
  // minor and channel are logically 32-bit (minor even 20 bit) but define them here as 64 bit to more cleanly calculate the key
  uint64_t minor;
  uint64_t channel;
  uint64_t key;
  message* to_write;

  if (length == 0 || length > 128)
  {
    return -EMSGSIZE;
  }
  
  channel = (uint64_t) file->private_data;
  if (channel == 0)
  {
    return -EINVAL;
  }

  minor = (uint64_t) iminor(file->f_inode);
  key = (minor << 32) | channel;
  to_write = kmalloc(sizeof(message), GFP_KERNEL);
  if (to_write == NULL)
  {
    return -ENOMEM;
  }

  printk("writing to minor %llu and channel %llu", minor, channel);
  printk("message length is %lu, message to write is: %.*s", length, (int)length, buffer);

  to_write->len = length;
  if (copy_from_user(to_write->contents, buffer, length) != 0)
  {
    kfree(to_write);
    return -EINVAL; // user provided bad buffer
  }
  
  if (ht_set(messages, key, to_write) == 0)
  {
    kfree(to_write);
    return -ENOMEM; // failure in set means failure in expand due to no memory
  }

  return length;
}

//----------------------------------------------------------------
static long device_ioctl( struct   file* file,
                          unsigned int   ioctl_command_id,
                          unsigned long  ioctl_param )
{
  if (ioctl_command_id == MSG_SLOT_CHANNEL)
  {
    if (ioctl_param == 0)
    {
      return -EINVAL;
    }
    else
    {
      printk("Invoking ioctl - setting channel to %ld\n", ioctl_param);
      file->private_data = (void *)ioctl_param;
      return SUCCESS;
    }
  }
  else
  {
    return -EINVAL;
  }
}

//==================== DEVICE SETUP =============================

struct file_operations Fops = {
  .owner	  = THIS_MODULE, 
  .read           = device_read,
  .write          = device_write,
  .open           = device_open,
  .unlocked_ioctl = device_ioctl,
  .release        = device_release,
};

//---------------------------------------------------------------
static int __init dev_init(void)
{
  int rc = -1;

  // Register driver capabilities. Obtain major num
  rc = register_chrdev( MAJOR_NUM, DEVICE_RANGE_NAME, &Fops );

  // Negative values signify an error
  if( rc < 0 ) {
    printk( KERN_ERR "message_slot: registraion failed for  %d\n", MAJOR_NUM );
    return rc;
  }

  messages = ht_create();
  if (messages == NULL)
  {
    printk( KERN_ERR "message_slot: creating hash table failed due to not enough memory");
    return -ENOMEM;
  }

  printk("message_slot: initialization is successful.");

  return 0;
}

//---------------------------------------------------------------
static void __exit dev_cleanup(void)
{
  ht_destroy(messages);
  unregister_chrdev(MAJOR_NUM, DEVICE_RANGE_NAME);
}

//---------------------------------------------------------------
module_init(dev_init);
module_exit(dev_cleanup);

//========================= END OF FILE =========================
