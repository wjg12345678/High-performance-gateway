#ifndef HTTP_RING_BUFFER_H
#define HTTP_RING_BUFFER_H

struct ring_buffer
{
    char *buffer;
    int capacity;
    int head;
    int tail;
    int size;
};

#endif
