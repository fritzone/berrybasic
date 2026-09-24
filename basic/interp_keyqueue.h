#ifndef INTERP_KEYQUEUE_H
#define INTERP_KEYQUEUE_H

#include "interp_keyqueue.h"

// A buffer for keys that poll_events consumed while checking for ^C and ON KEY,
// held for the handler (or the next GET) to read. GET/INKEY go through these
// wrappers so that buffered keys don't get lost and, for ON KEY events, the key
// that triggered the event is the one the handler reads back.

void key_queue_enqueue(int key);
int key_queue_dequeue();
int key_queue_avail();
void key_queue_clear();

#endif