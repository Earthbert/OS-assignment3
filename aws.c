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

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
    STATE_WAITING_FOR_DATA,
    STATE_DATA_RECEIVED,
    STATE_DATA_SENT,
    STATE_CONNECTION_CLOSED
};


/* structure acting as a connection handler */
struct connection {
    int sockfd;
    /* buffers used for receiving messages and then echoing them back */
    char recv_buffer[BUFSIZ];
    size_t recv_len;
    char send_buffer[BUFSIZ];
    size_t send_len;
    enum connection_state state;
};

typedef struct connection connection_t;

static connection_t *connection_create(int sockfd) {
    connection_t *conn = calloc(1, sizeof(*conn));
    DIE(!conn, "calloc");
    conn->sockfd = sockfd;
    conn->state = STATE_WAITING_FOR_DATA;
    return conn;
}

static void handle_new_connection(void) {
    int sockfd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in addr;
    connection_t *conn;
    int rc;

    /* accept new connection */
    sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
    DIE(sockfd < 0, "accept");

    dlog(LOG_INFO, "Accepted connection from: %s:%d\n",
         inet_ntoa(addr.sin_addr), ntohs(addr.sin_port))

    /* instantiate new connection handler */
    conn = connection_create(sockfd);

    /* add socket to epoll */
    rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
    DIE(rc < 0, "w_epoll_add_in");
}

static void connection_remove(struct connection *conn)
{
    int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr");
    close(conn->sockfd);
    conn->state = STATE_CONNECTION_CLOSED;
    free(conn);
}

static enum connection_state receive_message(connection_t *conn) {
    ssize_t bytes_recv = 0;
    int rc;
    char addr_buffer[64];

    rc = get_peer_address(conn->sockfd, addr_buffer, 64);
    if (rc < 0) {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_recv += recv(conn->sockfd, conn->recv_buffer, BUFSIZ - bytes_recv, 0);
    if (bytes_recv < 0) {        /* error in communication */
        dlog(LOG_ERR, "Error in communication from: %s\n", addr_buffer);
        goto remove_connection;
    }
    if (bytes_recv == 0) {        /* connection closed */
        dlog(LOG_INFO, "Connection closed from: %s\n", addr_buffer);
        goto remove_connection;
    }

    dlog(LOG_INFO, "Received message from: %s\n", addr_buffer)
    dlog(LOG_DEBUG, "--%s--\n", conn->recv_buffer)

    conn->recv_len = bytes_recv;
    conn->state = STATE_DATA_RECEIVED;

    return STATE_DATA_RECEIVED;

    remove_connection:
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr");

    /* remove current connection */
    connection_remove(conn);

    return STATE_CONNECTION_CLOSED;
}

static void handle_client_request(struct connection *conn) {
    int rc;
    enum connection_state ret_state;

    ret_state = receive_message(conn);
    if (ret_state == STATE_CONNECTION_CLOSED)
        return;

    connection_copy_buffers(conn);

    /* add socket to epoll for out events */
    rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_add_ptr_inout");
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
    ssize_t bytes_sent;
    int rc;
    char addr_buffer[64];

    rc = get_peer_address(conn->sockfd, addr_buffer, 64);
    if (rc < 0) {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_sent = send(conn->sockfd, conn->send_buffer, conn->send_len, 0);
    if (bytes_sent < 0) {		/* error in communication */
        dlog(LOG_ERR, "Error in communication to %s\n", addr_buffer)
        goto remove_connection;
    }
    if (bytes_sent == 0) {		/* connection closed */
        dlog(LOG_INFO, "Connection closed to %s\n", addr_buffer)
        goto remove_connection;
    }

    dlog(LOG_INFO, "Sending message to %s\n", addr_buffer)
    dlog(LOG_DEBUG, "--\n%s--\n", conn->send_buffer)

    /* all done - remove out notification */
    rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_update_ptr_in");

    conn->state = STATE_DATA_SENT;

    return STATE_DATA_SENT;

    remove_connection:
    /* remove current connection */
    connection_remove(conn);

    return STATE_CONNECTION_CLOSED;
}

int main() {
    listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

    epollfd = w_epoll_create();
    DIE(epollfd < 0, "w_epoll_create");

    int rc = w_epoll_add_fd_in(epollfd, listenfd);
    DIE(rc < 0, "w_epoll_add_fd_in");

    dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT)

    while (1) {
        struct epoll_event epollev;
        rc = w_epoll_wait_infinite(epollfd, &epollev);
        DIE(rc < 0, "w_epoll_wait_infinite");

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
            }
        }
    }
}
