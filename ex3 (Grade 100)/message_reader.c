#include "message_slot.h"    

#include <fcntl.h>      /* open */ 
#include <unistd.h>     /* exit */
#include <sys/ioctl.h>  /* ioctl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
  int file_desc;
  int ret_val;
  char msg[128];
  int bytes_read;

  if (argc != 3) {
    fprintf(stderr, "Invalid arguments\n");
    return 1;
  }

  file_desc = open( argv[1], O_RDWR );
  if( file_desc < 0 ) {
    perror("open failed");
    return 1;
  }

  if(ioctl( file_desc, MSG_SLOT_CHANNEL, atoi(argv[2])) < 0) {
    perror("ioctl failed");
    return 1;
  }

  bytes_read = read(file_desc, msg, 128);
  if (bytes_read < 0) {
    perror("read failed");
    return 1;
  }

  if (write(1, msg, bytes_read) < 0) {
    perror("write failed");
    return 1;
  }
 
  if(close(file_desc) < 0) {
    perror("close failed");
    return 1;
  }

  return 0;
}
