#ifndef MESSAGESLOT_H
#define MESSAGESLOT_H

#include <linux/ioctl.h>

#define MAJOR_NUM 235

// Set the channel
#define MSG_SLOT_CHANNEL _IOW(MAJOR_NUM, 0, unsigned long)

#define DEVICE_RANGE_NAME "message_slot"
#define SUCCESS 0

#endif
