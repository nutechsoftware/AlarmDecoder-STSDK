/**
*  @file    ser2sock.cpp
*  @author  Sean Mathews <coder@f34r.com>
*  @date    12/17/2020
*  @version 1.0.0
*
*  @brief ser2sock server daemon
*
*  @copyright Copyright (C) 2020 Nu Tech Software Solutions, Inc.
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
*/

// common includes
// stdc
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <ctype.h>

// stdc++
#include <string>
#include <sstream>

// esp includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <lwip/netdb.h>
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "SER2SOCKD";

// AlarmDecoder includes
#include "alarmdecoder_main.h"
#include "ad2_utils.h"
#include "ad2_settings.h"
#include "ad2_uart_cli.h"
#include "device_control.h"

// Disable via sdkconfig
#if CONFIG_SER2SOCKD

// specific includes
#include "ser2sock.h"
#ifdef __cplusplus
extern "C" {
#endif

// types and structs
enum FD_TYPES {
    NA, LISTEN_SOCKET = 1, CLIENT_SOCKET
} fd_types;

typedef struct {
    int size, in, out, avail;
    void **table;
} fifo;

typedef struct {
    int size;
    unsigned char buffer[];
} fifo_buffer;

typedef struct {
    /* flags */
    int inuse;
    int fd_type;

    /* the fd */
    int fd;

    /* the buffer */
    fifo send_buffer;
} FDs;

// fifo buffer stuff
static void _fifo_init(fifo *f, int size);
static void _fifo_destroy(fifo *f);
static int _fifo_empty(fifo *f);
static void* _fifo_make_buffer(void *in_buffer, unsigned int len);
static int _fifo_add(fifo *f, void *next);
static void* _fifo_get(fifo *f);
static void _fifo_clear(fifo *f);

int socket_timeout = 10;
int listen_backlog = 10;

FDs my_fds[MAXCONNECTIONS];

/* our listen socket */
int listen_sock = -1;
struct sockaddr_in serv_addr;
struct sockaddr_in peer_addr;

/**
 * ser2sock command list and enum.
 */
char * S2SD_SUBCMD [] = {
    (char*)S2SD_SUBCMD_ENABLE,
    (char*)S2SD_SUBCMD_ACL,
    0 // EOF
};

enum {
    S2SD_SUBCMD_ENABLE_ID = 0,
    S2SD_SUBCMD_ACL_ID,
};

/**
 * ser2sockd generic command event processing
 *  command: [COMMAND] <id> <arg>
 * ex.
 *   [COMMAND] 0 arg...
 */
static void _cli_cmd_ser2sockd_event(char *string)
{

    // key value validation
    std::string cmd;
    ad2_copy_nth_arg(cmd, string, 0);
    ad2_lcase(cmd);

    if(cmd.compare(SD2D_COMMAND) != 0) {
        ad2_printf_host("What?\r\n");
        return;;
    }

    // key value validation
    std::string subcmd;
    ad2_copy_nth_arg(subcmd, string, 1);
    ad2_lcase(subcmd);

    int i;
    for(i = 0;; ++i) {
        if (S2SD_SUBCMD[i] == 0) {
            ad2_printf_host("What?\r\n");
            break;
        }
        if(subcmd.compare(S2SD_SUBCMD[i]) == 0) {
            std::string arg;
            switch(i) {
            /**
             * Enable/Disable ser2sock daemon.
             */
            case S2SD_SUBCMD_ENABLE_ID:
                ESP_LOGI(TAG, "%s: enable/disable " SD2D_COMMAND, __func__);
                if (ad2_copy_nth_arg(arg, string, 2) >= 0) {
                    ad2_set_nv_slot_key_int(SD2D_COMMAND, S2SD_SUBCMD_ENABLE_ID, nullptr, (arg[0] == 'Y' || arg[0] ==  'y'));
                    ad2_printf_host("Success setting value. Restart required to take effect.\r\n");
                }

                // show contents of this slot
                int i;
                ad2_get_nv_slot_key_int(SD2D_COMMAND, S2SD_SUBCMD_ENABLE_ID, nullptr, &i);
                ad2_printf_host("ser2sock daemon is '%s'.\r\n", (i ? "Enabled" : "Disabled"));
                break;
            /**
             * ser2sock daemon IP/CIDR ACL list.
             */
            case S2SD_SUBCMD_ACL_ID:
                ad2_printf_host("Not yet implemented.\r\n");
                break;
            default:
                break;
            }
            break;
        }
    }
}

/**
 * @brief command list for module
 */
static struct cli_command ser2sockd_cmd_list[] = {
    {
        (char*)SD2D_COMMAND,(char*)
        "- ser2sock daemon component command\r\n"
        "  ```" SD2D_COMMAND " {sub command} {arg}```\r\n"
        "  - {sub command}\r\n"
        "    - [" S2SD_SUBCMD_ENABLE "] Enable / Disable ser2sock daemon\r\n"
        "      -  {arg1}: [Y]es [N]o\r\n"
        "        - [N] Default state\r\n"
        "        - Example: " SD2D_COMMAND " " S2SD_SUBCMD_ENABLE " Y\r\n"
        "    - [" S2SD_SUBCMD_ACL "] Set / Get ACL list\r\n"
        "      - {arg1}: ACL LIST\r\n"
        "      -  String of CIDR values seperated by commas.\r\n"
        "        - Default: Empty string disables ACL list\r\n"
        "        - Example: " SD2D_COMMAND " " S2SD_SUBCMD_ACL " 192.168.0.123/32,192.168.1.0/24\r\n\r\n", _cli_cmd_ser2sockd_event
    }
};

/**
 * @brief Register componet cli commands.
 */
void ser2sockd_register_cmds()
{
    // Register ser2sock CLI commands
    for (int i = 0; i < ARRAY_SIZE(ser2sockd_cmd_list); i++) {
        cli_register_command(&ser2sockd_cmd_list[i]);
    }
}

/**
 * @brief Initialize the ser2sock daemon
 */
void ser2sockd_init(void)
{
    int enabled = 0;
    ad2_get_nv_slot_key_int(SD2D_COMMAND, S2SD_SUBCMD_ENABLE_ID, nullptr, &enabled);

    // nothing more needs to be done once commands are set if not enabled.
    if (!enabled) {
        ESP_LOGI(TAG, "ser2sockd disabled");
        return;
    }

    int x;
    for (x = 0; x < MAXCONNECTIONS; x++) {
        my_fds[x].inuse = false;
        my_fds[x].fd = -1;
        my_fds[x].fd_type = NA;
        _fifo_init(&my_fds[x].send_buffer, MAX_FIFO_BUFFERS);
    }

    ESP_LOGI(TAG, "Starting ser2sockd");
    xTaskCreate(&ser2sockd_server_task, "ser2sockd_server_task", 1024*5, NULL, tskIDLE_PRIORITY+1, NULL);

}


//<Fifo Buffer>
/*
 init queue allocate memory
 */
static void _fifo_init(fifo *f, int size)
{
    f->avail = 0;
    f->in = 0;
    f->out = 0;
    f->size = size;
    f->table = (void**) malloc(f->size * sizeof(void*));
}

/*
 fifo empty if queue = 1 else 0
 */
static int _fifo_empty(fifo *f)
{
    return (f->avail == 0);
}

/*
 free up any memory we allocated
 */
static void _fifo_destroy(fifo *f)
{
    int i;
    if (!_fifo_empty(f)) {
        free(f->table);
    } else {
        for (i = f->out; i < f->in; i++) {
            /* free actual block of memory */
            free(f->table[i]);
        }
        free(f->table);
    }
}

/*
 remove all stored pending data
 */
static void _fifo_clear(fifo *f)
{
    void *p;
    while (!_fifo_empty(f)) {
        p = _fifo_get(f);
        if (p) {
            free(p);
        }
    }
}

/*
 allocate a fifo_buffer and fill it with the supplied in_buffer and len.
 if len is 0 it will be calculated using strlen() so NULL chars in the
 stream will be excluded and thus not true binary. For binary and to include
 NULL values in the stream len must be > 0.
 note:
	this has a specific type where all the other fifo low level routines
	are all void *.
 */
static void* _fifo_make_buffer(void *in_buffer, unsigned int len)
{
    fifo_buffer* out_buffer;

    // Calculate the buffer size as a string not including the null terminator
    if (!len) {
        len = strlen((const char *)in_buffer);
    }
    out_buffer = (fifo_buffer *)malloc(sizeof(out_buffer->size)+len);
    memcpy(out_buffer->buffer, in_buffer, len);
    out_buffer->size = len;
    return (void*) out_buffer;
}

/*
 insert an element
 this must be already allocated with malloc or strdup
 */
static int _fifo_add(fifo *f, void *next)
{
    if (f->avail == f->size) {
        return (0);
    } else {
        f->table[f->in] = next;
        f->avail++;
        f->in = (f->in + 1) % f->size;
        return (1);
    }
}

/*
 return next element
 */
static void* _fifo_get(fifo *f)
{
    void* get;
    if (f->avail > 0) {
        get = f->table[f->out];
        f->out = (f->out + 1) % f->size;
        f->avail--;
        return (get);
    }
    return 0;
}
//</Fifo Buffer>

/*
 Cleanup an entry in the fd array and do any fd_type specific cleanup
 */
static int _cleanup_fd(int n)
{

    /* don't do anything unless its in was active */
    if (my_fds[n].inuse) {
        /* close the fd */
        close(my_fds[n].fd);
        my_fds[n].fd = -1;

        /* clear any data we have saved */
        _fifo_clear(&my_fds[n].send_buffer);

        /* mark the element as free for reuse */
        my_fds[n].inuse = false;

        /* set the type to null */
        my_fds[n].fd_type = NA;

    }
    return true;
}

/*
 ser2sockd_sendall
 adds a buffer to every connected socket fd ie multiplexes
 */
void ser2sockd_sendall(uint8_t *buffer, size_t len)
{
    void * tempbuffer;
    int n;

    /*
     Adding anything to the fifo must be allocated so it can be free'd later
     Not very efficient but we have plenty of mem with as few connections as we
     will use. If we needed many more I would need to re-factor this code
     */
    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse == true) {
            if (my_fds[n].fd_type == CLIENT_SOCKET) {
                /* caller of fifo_get must free this */
                tempbuffer = _fifo_make_buffer(buffer, len);
                _fifo_add(&my_fds[n].send_buffer, tempbuffer);
            }
        }
    }
}

/*
 add all of our fd to our r,w and e fd sets
*/
static void _build_fdsets(fd_set *read_fdset, fd_set *write_fdset, fd_set *except_fdset)
{
    int n;

    /* add all sockets to our fdset */
    FD_ZERO(read_fdset);
    FD_ZERO(write_fdset);
    FD_ZERO(except_fdset);
    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse == true) {
            FD_SET(my_fds[n].fd,read_fdset);
            FD_SET(my_fds[n].fd,write_fdset);
            FD_SET(my_fds[n].fd,except_fdset);
        }
    }
}

/*
 poll any exception fd's return TRUE if we did some work
 */
static bool _poll_exception_fdset(fd_set *except_fdset)
{
    int n;
    bool did_work = false;

    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse == true) {
            if (FD_ISSET(my_fds[n].fd,except_fdset)) {
                if (my_fds[n].fd_type == CLIENT_SOCKET) {
                    did_work = true;
                    ESP_LOGE(TAG, "Exception occured on socket fd slot %i closing the socket.",n);
                    _cleanup_fd(n);
                }
            }
        }
    }
    return did_work;
}

/*
 Makes a fd non blocking
 */
static void _set_non_blocking(int fd)
{
    int nonb = 0;
    int res = 1;
    nonb |= O_NONBLOCK;
    if (ioctl(fd, FIONBIO, &res) < 0) {
        ESP_LOGE(TAG, "Error setting FIONBIO");
    }
}

/*
 Add a fd to our array so we can poll it in our state machien loop
 */
static int _add_fd(int fd, int fd_type)
{
    int x;
    int results = -1;
    struct linger solinger;

    for (x = 0; x < MAXCONNECTIONS; x++) {
        if (my_fds[x].inuse == false) {
            solinger.l_onoff = true;
            solinger.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &solinger, sizeof(solinger));

            if (fd_type == CLIENT_SOCKET) {
                int ret;
                int keep_alive = 1;
                ret = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int));
                if (ret < 0) {
                    ESP_LOGI(TAG, "socket set keep-alive failed %d", errno);
                }

                int idle = 10;
                ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(int));
                if (ret < 0) {
                    ESP_LOGI(TAG, "socket set keep-idle failed %d", errno);
                }

                int interval = 5;
                ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(int));
                if (ret < 0) {
                    ESP_LOGI(TAG, "socket set keep-interval failed %d", errno);
                }

                int maxpkt = 3;
                ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(int));
                if (ret < 0) {
                    ESP_LOGI(TAG, "socket set keep-count failed %d", errno);
                }
            }

            my_fds[x].inuse = true;
            my_fds[x].fd_type = fd_type;
            my_fds[x].fd = fd;
            results = x;
            break;
        }
    }

    return results;
}

/*
  poll any read fd's return true if we did do some work
 */
static bool _poll_read_fdset(fd_set *read_fdset)
{
    int n, received, newsockfd, added_slot;
    unsigned int clilen;
    bool did_work = false;
    char buffer[1024] = {0};

    clilen = sizeof(struct sockaddr_in);

    /* check every socket to find the one that needs read */
    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse == true) {

            /* check read fd */
            if (FD_ISSET(my_fds[n].fd,read_fdset)) {
                /*  if this is a listening socket then we accept on it and
                 * get a new client socket
                 */
                if (my_fds[n].fd_type == LISTEN_SOCKET) {
                    /* clear our state vars */
                    newsockfd = -1;
                    {
                        newsockfd = accept(listen_sock, (struct sockaddr *) &peer_addr, &clilen);
                    }
                    if (newsockfd != -1) {
                        /* reset our added id to a bad state */
                        added_slot = -2;
                        added_slot = _add_fd(newsockfd, CLIENT_SOCKET);
                        if (added_slot >= 0) {
                            ESP_LOGI(TAG, "Socket connected slot %i from %s", added_slot, inet_ntoa(peer_addr.sin_addr));
                            did_work = true;
                        } else {
                            ESP_LOGI(TAG,"add slot error %i", added_slot);
                            close(newsockfd);
                            if(added_slot == -1) {
                                ESP_LOGI(TAG, "Socket refused because no more space");
                            }
                        }
                    } else {
                        ESP_LOGI(TAG,"accept errno: %i '%s' %i", errno, strerror(errno), listen_sock);
                    }
                } else {
                    errno = 0;
                    {
                        received = recv(my_fds[n].fd, (void *)buffer, sizeof(buffer), 0);
                    }
                    if (received == 0) {
                        ESP_LOGI(TAG, "Closing socket fd slot %i errno: %i '%s'", n,
                                 errno, strerror(errno));
                        _cleanup_fd(n);
                    } else {
                        if (received < 0) {
                            if (errno == EAGAIN || errno == EINTR) {
                                continue;
                            }
                            ESP_LOGI(TAG,
                                     "Closing socket errno: %i '%s'",
                                     errno, strerror(errno));
                            _cleanup_fd(n);
                        } else {
                            did_work = true;
                            // FIXME: Need to keep it clean and not call back into main()
                            ESP_LOGI(TAG,"fd(%i) slot(%i) sending %i bytes to the AD2*", my_fds[n].fd, n, received);
                            // FIXME: overide to send raw pointer and not buffer.
                            std::string tmp(buffer, received);
                            ad2_send(tmp);
                        }
                    }
                }
            } /* end FD_ISSET() */
        }
    }

    return did_work;
}

/*
  poll all write fd's return TRUE if we did do some work
 */
static bool _poll_write_fdset(fd_set *write_fdset)
{
    int n;
    fifo_buffer* tempbuffer;
    bool did_work = false;

    /* check every socket to find the one that needs write */
    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse == true && FD_ISSET(my_fds[n].fd,write_fdset)) {
            /* see if we have data to write */
            if (!_fifo_empty(&my_fds[n].send_buffer)) {
                /* set our var to an invalid state */
                tempbuffer = NULL;

                /* handle writing to CLIENT_SOCKET */
                if (my_fds[n].fd_type == CLIENT_SOCKET) {
                    /* load our buffer with data to send */
                    {
                        tempbuffer = (fifo_buffer *) _fifo_get(
                                         &my_fds[n].send_buffer);
                        send(my_fds[n].fd, tempbuffer->buffer, tempbuffer->size, 0);
                    }

                    /* did we do any work? */
                    if ( tempbuffer ) {
                        did_work = true;
                    }
                }
                /* free up memory */
                if(tempbuffer) {
                    free(tempbuffer);
                }
            }
        }
    }

    return did_work;
}

/**
 * @brief ser2sock server task
 *
 * @param [in]pvParameters currently not used NULL.
 */
void ser2sockd_server_task(void *pvParameters)
{
    int addr_family = AF_INET;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;
    int n;
    bool bOptionTrue = true;
    bool did_work = false;
    fd_set read_fdset, write_fdset, except_fdset;
    struct timeval wait;


    ESP_LOGI(TAG, "ser2sock server task starting.");

    for (;;) {
        if (g_ad2_network_state == AD2_CONNECTED) {
            ESP_LOGI(TAG, "network up creating listening socket");
            if (addr_family == AF_INET) {
                struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
                dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
                dest_addr_ip4->sin_family = AF_INET;
                dest_addr_ip4->sin_port = htons(PORT);
                ip_protocol = IPPROTO_IP;
            } else if (addr_family == AF_INET6) {
                bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
                dest_addr.sin6_family = AF_INET6;
                dest_addr.sin6_port = htons(PORT);
                ip_protocol = IPPROTO_IPV6;
            }

            listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
            if (listen_sock < 0) {
                ESP_LOGE(TAG, "ser2sock server unable to create socket: errno %d", errno);
                vTaskDelete(NULL);
                return;
            }

#if defined(SER2SOCK_IPV4) && defined(SER2SOCK_IPV6)
            // Note that by default IPV6 binds to both protocols, it is must be disabled
            // if both protocols used at the same time (used in CI)
            setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &bOptionTrue, sizeof(bOptionTrue));
#endif
            setsockopt(listen_sock, SOL_SOCKET, SO_SNDTIMEO,
                       (char *) &socket_timeout, sizeof(socket_timeout));
            setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO,
                       (char *) &socket_timeout, sizeof(socket_timeout));
            setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &bOptionTrue,
                       sizeof(bOptionTrue));

            struct linger solinger;
            solinger.l_onoff = true;
            solinger.l_linger = 0;
            setsockopt(listen_sock, SOL_SOCKET, SO_LINGER, &solinger, sizeof(solinger));

            ESP_LOGI(TAG, "ser2sock server socket created %i", listen_sock);

            int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err != 0) {
                ESP_LOGE(TAG, "ser2sock server socket unable to bind: errno %d", errno);
                ESP_LOGE(TAG, "ser2sock server IPPROTO: %d", addr_family);
                goto CLEAN_UP;
            }
            ESP_LOGI(TAG, "ser2sock server socket bound, port %d", PORT);

            err = listen(listen_sock, 1);
            if (err != 0) {
                ESP_LOGE(TAG, "ser2sock server error occurred during listen: errno %d", errno);
                goto CLEAN_UP;
            }

            _set_non_blocking(listen_sock);

            _add_fd(listen_sock, LISTEN_SOCKET);

            while (1) {
                /* reset our loop state var(s) for this iteration */
                did_work = false;

                /* build our fd sets */
                _build_fdsets(&read_fdset, &write_fdset, &except_fdset);

                /* lets not block our select and bail after 20us */
                wait.tv_sec = 0;
                wait.tv_usec = 20;

                /* see if any of the fd's need attention */
                n = select(FD_SETSIZE, &read_fdset, &write_fdset, &except_fdset, &wait);
                if (n == -1) {
                    ESP_LOGE(TAG, "An error occurred during select() errno: %i '%s'", errno, strerror(errno));
                    continue;
                }
                /* poll our exception fdset */
                _poll_exception_fdset(&except_fdset);

                /* poll our read fdset */
                did_work = _poll_read_fdset(&read_fdset);

                /* poll our write fdset */
                did_work = _poll_write_fdset(&write_fdset);

                /* if we did not do anything then sleep a little predict
                next go round will be idle too
                */
                if (!did_work) {
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                }
            }

CLEAN_UP:
            close(listen_sock);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

#ifdef __cplusplus
} // extern "C"
#endif
#endif /*  CONFIG_SER2SOCKD */
