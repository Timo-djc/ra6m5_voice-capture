#include "uart_ringbuf.h"

static size_t rb_next(size_t idx, size_t cap)
{
    idx++;
    if (idx >= cap)
    {
        idx = 0U;
    }
    return idx;
}

void uart_ringbuf_init(uart_ringbuf_t * rb, volatile uint8_t * storage, size_t capacity)
{
    if ((NULL == rb) || (NULL == storage) || (capacity < 2U))
    {
        return;
    }

    rb->buf = storage;
    rb->capacity = capacity;
    rb->head = 0U;
    rb->tail = 0U;
    rb->overflow_count = 0U;
}

bool uart_ringbuf_push(uart_ringbuf_t * rb, uint8_t byte)
{
    size_t next;

    if ((NULL == rb) || (NULL == rb->buf) || (rb->capacity < 2U))
    {
        return false;
    }

    next = rb_next(rb->head, rb->capacity);
    if (next == rb->tail)
    {
        rb->tail = rb_next(rb->tail, rb->capacity);
        rb->overflow_count++;
    }

    rb->buf[rb->head] = byte;
    rb->head = next;
    return true;
}

bool uart_ringbuf_pop(uart_ringbuf_t * rb, uint8_t * out)
{
    if ((NULL == rb) || (NULL == rb->buf) || (NULL == out))
    {
        return false;
    }

    if (rb->tail == rb->head)
    {
        return false;
    }

    *out = rb->buf[rb->tail];
    rb->tail = rb_next(rb->tail, rb->capacity);
    return true;
}

bool uart_ringbuf_peek(const uart_ringbuf_t * rb, uint8_t * out)
{
    if ((NULL == rb) || (NULL == rb->buf) || (NULL == out))
    {
        return false;
    }

    if (rb->tail == rb->head)
    {
        return false;
    }

    *out = rb->buf[rb->tail];
    return true;
}

size_t uart_ringbuf_available(const uart_ringbuf_t * rb)
{
    size_t head;
    size_t tail;

    if ((NULL == rb) || (NULL == rb->buf))
    {
        return 0U;
    }

    head = rb->head;
    tail = rb->tail;

    if (head >= tail)
    {
        return head - tail;
    }

    return (rb->capacity - tail) + head;
}

size_t uart_ringbuf_read(uart_ringbuf_t * rb, uint8_t * out, size_t max_len)
{
    size_t i;

    if ((NULL == rb) || (NULL == rb->buf) || (NULL == out) || (0U == max_len))
    {
        return 0U;
    }

    for (i = 0U; i < max_len; i++)
    {
        if (!uart_ringbuf_pop(rb, &out[i]))
        {
            break;
        }
    }

    return i;
}

void uart_ringbuf_clear(uart_ringbuf_t * rb)
{
    if (NULL == rb)
    {
        return;
    }

    rb->head = 0U;
    rb->tail = 0U;
}

size_t uart_ringbuf_overflow_count(const uart_ringbuf_t * rb)
{
    if (NULL == rb)
    {
        return 0U;
    }

    return rb->overflow_count;
}
