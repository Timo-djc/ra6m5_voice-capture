#ifndef MVP_UART_RINGBUF_H_
#define MVP_UART_RINGBUF_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct st_uart_ringbuf
{
    volatile uint8_t * buf;
    size_t capacity;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t overflow_count;
} uart_ringbuf_t;

void uart_ringbuf_init(uart_ringbuf_t * rb, volatile uint8_t * storage, size_t capacity);
bool uart_ringbuf_push(uart_ringbuf_t * rb, uint8_t byte);
bool uart_ringbuf_pop(uart_ringbuf_t * rb, uint8_t * out);
bool uart_ringbuf_peek(const uart_ringbuf_t * rb, uint8_t * out);
size_t uart_ringbuf_available(const uart_ringbuf_t * rb);
size_t uart_ringbuf_read(uart_ringbuf_t * rb, uint8_t * out, size_t max_len);
void uart_ringbuf_clear(uart_ringbuf_t * rb);
size_t uart_ringbuf_overflow_count(const uart_ringbuf_t * rb);

#endif
