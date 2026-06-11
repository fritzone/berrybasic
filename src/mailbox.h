#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

// Shared mailbox buffer (36 words, 16-byte aligned)
extern volatile uint32_t mbox[36];

// Send mbox[] to property channel (ch=8) and wait for response.
// Returns 1 on success (mbox[1] == 0x80000000), 0 on failure.
int mbox_call(void);

#endif
