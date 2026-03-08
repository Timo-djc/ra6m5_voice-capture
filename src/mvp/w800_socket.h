#ifndef MVP_W800_SOCKET_H_
#define MVP_W800_SOCKET_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum e_w800_socket_link_status
{
    W800_SOCKET_STATUS_DISCONNECTED = 0,
    W800_SOCKET_STATUS_LISTEN = 1,
    W800_SOCKET_STATUS_CONNECTED = 2
} w800_socket_link_status_t;

typedef struct st_w800_socket_status
{
    int socket_id;
    int status;
    char remote_host[64];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t rx_data_len;
} w800_socket_status_t;

int w800_socket_open_tcp(const char * host, uint16_t remote_port, uint16_t local_port, int * out_socket);
int w800_socket_open(const char * host, uint16_t remote_port, uint16_t local_port, int * out_socket);
int w800_socket_send(int socket, const uint8_t * data, size_t size);
int w800_socket_recv(int socket, uint8_t * buf, size_t maxsize, size_t * out_size);
int w800_socket_get_status(int socket, w800_socket_status_t * st);
int w800_socket_close(int socket);
int w800_socket_set_report_mode(int mode);
int w800_socket_is_open(int socket);

#endif
