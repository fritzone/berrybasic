#include "interp_base.h"

// A buffer for keys that poll_events consumed while checking for ^C and ON KEY,
// held for the handler (or the next GET) to read. GET/INKEY go through these
// wrappers so that buffered keys don't get lost and, for ON KEY events, the key
// that triggered the event is the one the handler reads back.

static int pending_keys[KEY_QLEN];
static int pending_key_head = 0;
static int pending_key_tail = 0;

void key_queue_enqueue(int key) {
    int next_tail = (pending_key_tail + 1) % KEY_QLEN;

    if (next_tail == pending_key_head) return; // queue full

    pending_keys[pending_key_tail] = key;
    pending_key_tail = next_tail;
}

int key_queue_dequeue() {
    if (pending_key_head == pending_key_tail) return -1; // queue empty

    int key = pending_keys[pending_key_head];

    pending_key_head = (pending_key_head + 1) % KEY_QLEN;

    return key;
}

int key_queue_avail() {
    // Have to reserve one spot, otherwise full and empty look the same (head == tail).
    return (pending_key_head + (KEY_QLEN - 1) - pending_key_tail) % KEY_QLEN;
}

void key_queue_clear() {
    pending_key_head = pending_key_tail;
}

