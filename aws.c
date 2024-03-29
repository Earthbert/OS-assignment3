#include <stdlib.h>

#include <sys/sendfile.h>
#include <sys/epoll.h>
#include <libaio.h>

#include "util/http-parser/http_parser.h"
#include "util/util.h"
#include "aws.h"
#include "w_epoll.h"
#include "sock_util.h"
#include "util/debug.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <assert.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <sys/eventfd.h>

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static http_parser request_parser;
static char request_path[BUFSIZ];    /* storage for request_path */

enum request_type {
    TYPE_STATIC,
    TYPE_DYNAMIC
};

enum connection_state {
    STATE_WAITING_FOR_DATA,
    STATE_RECEIVING_DATA,
    STATE_DATA_RECEIVED,
    STATE_DATA_SENT,
    STATE_SENDING_DATA,
    STATE_CONNECTION_CLOSED,
    STATE_DYNAMIC_SENDING
};

/* structure acting as a connection handler */
struct connection {
    int sockfd;
    /* buffers used for receiving messages and then echoing them back */
    char recv_buffer[BUFSIZ];
    size_t recv_len;
    char send_buffer[BUFSIZ];
    size_t send_len;
    /* state of connection */
    enum connection_state state;
    /* path of requested file */
    char path[BUFSIZ];
    /* file descriptor and info of requested file */
    int req_fd;
    struct stat req_stat;
    /* nr of bytes from http header send */
    ssize_t send_bytes;
    /* nr of bytes of requested file send */
    ssize_t file_send_bytes;
    enum request_type type;
    int event_fd;
    char *file_buffer;
    io_context_t aio_ctx;
    struct iocb *iocb;
};

typedef struct connection connection_t;


/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */
static int on_path_cb(http_parser *p, const char *buf, size_t len) {
    assert(p == &request_parser);
    memcpy(request_path, buf, len);
    return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
        /* on_message_begin */ 0,
        /* on_header_field */ 0,
        /* on_header_value */ 0,
        /* on_path */ on_path_cb,
        /* on_url */ 0,
        /* on_fragment */ 0,
        /* on_query_string */ 0,
        /* on_body */ 0,
        /* on_headers_complete */ 0,
        /* on_message_complete */ 0
};

/*
 * Parse http request and store information about it in connection struct
 */
static void handle_received_http_request(connection_t *conn) {
    http_parser_init(&request_parser, HTTP_REQUEST);

    http_parser_execute(&request_parser, &settings_on_path,
                        conn->recv_buffer, strlen(conn->recv_buffer));

    strcat(conn->path, request_path);
    memset(request_path, 0, BUFSIZ);
    dlog(LOG_DEBUG, "Parsed HTTP request path: %s\n", conn->path)

    conn->req_fd = open(conn->path, O_RDONLY);
    if (conn->req_fd == -1) {
        dlog(LOG_ERR, "Cannot open file %s\n", conn->path)
    } else {
        fstat(conn->req_fd, &(conn->req_stat));
        if (strstr(conn->path, AWS_REL_STATIC_FOLDER)) {
            conn->type = TYPE_STATIC;
        } else {
            conn->type = TYPE_DYNAMIC;
        }
    }
}

/*
 * Initialize connection structure on given socket.
 */
static connection_t *connection_create(int sockfd) {
    connection_t *conn = calloc(1, sizeof(*conn));
    DIE(!conn, "calloc\n");
    conn->sockfd = sockfd;
    conn->state = STATE_WAITING_FOR_DATA;
    strcpy(conn->path, AWS_DOCUMENT_ROOT);
    return conn;
}

/*
 * Remove connection handler.
 */
static void connection_remove(struct connection *conn) {
    int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr\n");
    close(conn->sockfd);
    close(conn->req_fd);
    conn->state = STATE_CONNECTION_CLOSED;
    free(conn);
}

/*
 * Handle a client request on a client connection.
 */
static void handle_new_connection(void) {
    int sockfd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in addr;
    connection_t *conn;
    int rc;

    /* accept new connection */
    sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
    DIE(sockfd < 0, "accept\n");
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    dlog(LOG_INFO, "Accepted connection from: %s:%d\n",
         inet_ntoa(addr.sin_addr), ntohs(addr.sin_port))

    /* instantiate new connection handler */
    conn = connection_create(sockfd);

    /* add socket to epoll */
    rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
    DIE(rc < 0, "w_epoll_add_in\n");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */
static enum connection_state receive_message(connection_t *conn) {
    ssize_t bytes_recv;
    int rc;
    char addr_buffer[64];

    rc = get_peer_address(conn->sockfd, addr_buffer, 64);
    if (rc < 0) {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
    if (bytes_recv < 0) {        /* error in communication */
        dlog(LOG_ERR, "Error in communication from: %s\n", addr_buffer)
        goto remove_connection;
    }
    if (bytes_recv == 0) {        /* connection closed */
        dlog(LOG_INFO, "Connection closed from: %s\n", addr_buffer)
        goto remove_connection;
    }

    dlog(LOG_INFO, "Received message from: %s\n", addr_buffer)
    dlog(LOG_DEBUG, "--%s--\n", conn->recv_buffer)

    conn->recv_len += bytes_recv;
    conn->state = STATE_RECEIVING_DATA;

    return STATE_RECEIVING_DATA;

    remove_connection:
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr\n");

    /* remove current connection */
    connection_remove(conn);

    return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */
static void handle_client_request(struct connection *conn) {
    enum connection_state ret_state;

    if (conn->state != STATE_DATA_RECEIVED) {
        ret_state = receive_message(conn);
        if (ret_state == STATE_CONNECTION_CLOSED)
            return;
    }

    if (strstr(conn->recv_buffer, "\r\n\r\n") && conn->state == STATE_RECEIVING_DATA) {
        handle_received_http_request(conn);
        conn->state = STATE_DATA_RECEIVED;
        int rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
        DIE(rc < 0, "w_epoll_add_ptr_inout");
    }
}

/*
 * Prepare http header and store it in send_buffer
 */
static void prepare_send_message(struct connection *conn) {
    if (conn->req_fd == -1) {
        strcpy(conn->send_buffer, "HTTP/1.0 404 File no here\r\n\r\n");
        conn->send_len = strlen(conn->send_buffer);
    } else {
        strcpy(conn->send_buffer, "HTTP/1.0 200 File here\r\n\r\n");
        conn->send_len = strlen(conn->send_buffer);
    }
}

static void prepare_for_aio(connection_t *conn) {
    conn->file_buffer = calloc(1, conn->req_stat.st_size);
    conn->iocb = calloc(1, sizeof(*conn->iocb));
    io_prep_pread(conn->iocb, conn->req_fd, conn->file_buffer, conn->req_stat.st_size, 0);
    int rc = io_setup(1, &conn->aio_ctx);
    DIE(rc < 0, "io_setup");
    rc = io_submit(conn->aio_ctx, 1, &conn->iocb);
    DIE(rc < 0, "io_submit");
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */
static void  send_message(connection_t *conn) {
    ssize_t bytes_sent;
    int rc;
    char addr_buffer[64];

    rc = get_peer_address(conn->sockfd, addr_buffer, 64);
    if (rc < 0) {
        ERR("get_peer_address");
        goto remove_connection;
    }

    if (conn->state == STATE_DATA_RECEIVED) {
        prepare_send_message(conn);
    }

    if (conn->send_bytes < conn->send_len) {
        conn->state = STATE_SENDING_DATA;
        bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_bytes, conn->send_len - conn->send_bytes, 0);
        conn->send_bytes += bytes_sent;
        if (bytes_sent < 0) {        /* error in communication */
            dlog(LOG_ERR, "Error in communication to %s\n", addr_buffer)
            goto remove_connection;
        }
        if (bytes_sent == 0) {        /* connection closed */
            dlog(LOG_INFO, "Connection closed to %s\n", addr_buffer)
            goto remove_connection;
        }
        dlog(LOG_INFO, "Sending message to %s\n", addr_buffer)
        dlog(LOG_DEBUG, "--\n%s--\n", conn->send_buffer + conn->send_bytes)
        return;
    }

    if (conn->type == TYPE_STATIC) {
        if (conn->file_send_bytes < conn->req_stat.st_size) {
            dlog(LOG_INFO, "Sending data from file to %s\n", addr_buffer)
            conn->file_send_bytes += sendfile(conn->sockfd, conn->req_fd, 0, conn->req_stat.st_size);
            return;
        }
    } else if (conn->type == TYPE_DYNAMIC) {
        if (conn->state == STATE_SENDING_DATA) {
            prepare_for_aio(conn);
            conn->state = STATE_DYNAMIC_SENDING;
            return;
        } else if (conn->state == STATE_DYNAMIC_SENDING) {
            struct io_event io_event;
            if (io_getevents(conn->aio_ctx, 1, 1, &io_event, NULL)) {
                io_prep_pwrite(conn->iocb, conn->sockfd, conn->file_buffer, conn->req_stat.st_size, 0);
                rc = io_submit(conn->aio_ctx, 1, &conn->iocb);
                DIE(rc < 0, "io_submit");
                dlog(LOG_INFO, "Sending data from file to %s:\n", addr_buffer)
                conn->state = STATE_DATA_SENT;
            }
            return;
        } else {
            struct io_event io_event;
            if (io_getevents(conn->aio_ctx, 1, 1, &io_event, NULL)) {
                io_destroy(conn->aio_ctx);
            }
        }
    }

    /* all done - remove out notification */
    rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_update_ptr_in\n");

    dlog(LOG_INFO, "Finished sending data, closing connection\n")

    remove_connection:

    /* remove current connection */
    connection_remove(conn);
}

int main() {
    listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

    epollfd = w_epoll_create();
    DIE(epollfd < 0, "w_epoll_create\n");

    int rc = w_epoll_add_fd_in(epollfd, listenfd);
    DIE(rc < 0, "w_epoll_add_fd_in\n");

    dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT)

    while (1) {
        struct epoll_event epollev;
        rc = w_epoll_wait_infinite(epollfd, &epollev);
        DIE(rc < 0, "w_epoll_wait_infinite\n");

        if (epollev.data.fd == listenfd) {
            dlog(LOG_INFO, "New connection\n")
            if (epollev.events & EPOLLIN) {
                handle_new_connection();
            }
        } else {
            if (epollev.events & EPOLLIN) {
                dlog(LOG_INFO, "New message\n")
                handle_client_request(epollev.data.ptr);
            }
            if (epollev.events & EPOLLOUT) {
                dlog(LOG_INFO, "Ready to send message\n")
                send_message(epollev.data.ptr);
            }
        }
    }
}