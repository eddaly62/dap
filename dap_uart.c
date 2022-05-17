// dap_uart.c

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include "dap.h"

// Maximum number of events to be returned from a single epoll_wait() call
#define EP_MAX_EVENTS 5

// uart io multiplexing
static struct DAP_UART_EPOLL {
    int epfd;
    int numOpenFds;
    struct epoll_event ev;
    struct epoll_event evlist[EP_MAX_EVENTS];
    //TODO - add a semaphore used to signal app 
};

// UART data structures
static struct DAP_UART uart1;
static struct DAP_UART uart2;
static struct DAP_UART_EPOLL uep;
static pthread_t tid_uart;

// clear uart recieve buffer
void dap_port_clr_rx_buffer (struct DAP_UART *u) {
    memset(u->buf_rx, 0, sizeof(u->buf_rx));
    u->read_ptr = u->buf_rx;
    u->num_unread = 0;
}

// clear uart transmit buffer
void dap_port_clr_tx_buffer (struct DAP_UART *u) {
    memset(u->buf_tx, 0, sizeof(u->buf_tx));
    u->num_to_tx = 0;
}

// close uart
void dap_port_close (struct DAP_UART *u) {
    close (u->fd_uart);
    u->fd_uart = 0;
    dap_port_clr_rx_buffer (u);
    dap_port_clr_tx_buffer (u);
}

// set uart attributes (helper function to dap_port_init)
static int dap_port_init_attributes (struct DAP_UART *u) {

    if (u->fd_uart <= 0) {
        ASSERT(ASSERT_FAIL, "UART: fd_uart <= 0, can not set attributes", "-1")
        return -1;
    }

    tcgetattr(u->fd_uart, &u->tty);     // Get the current attributes of the first serial port

    cfsetispeed(&u->tty, u->baud);      // Set read speed
    cfsetospeed(&u->tty, u->baud);      // Set write speed

    u->tty.c_cflag &= ~PARENB;          // Disables the Parity Enable bit(PARENB)
    u->tty.c_cflag &= ~CSTOPB;          // Clear CSTOPB, configuring 1 stop bit
    u->tty.c_cflag &= ~CSIZE;           // Using mask to clear data size setting
    u->tty.c_cflag |= CS8;              // Set 8 data bits
    u->tty.c_cflag &= ~CRTSCTS;         // Disable Hardware Flow Control
                                        // TODO - Add Xon/Xoff flow control

    if ((tcsetattr(u->fd_uart, TCSANOW, &u->tty)) != 0) {   // Save configuration
        ASSERT(ASSERT_FAIL, "UART Initialization: Can not set UART attributes", strerror(errno))
        return DAP_ERROR;
    }
    return DAP_SUCCESS;
}


// initializes UART port
int dap_port_init (struct DAP_UART *u, char *upath, speed_t baud) {

    int results = DAP_SUCCESS;

    dap_port_clr_rx_buffer(u);
    dap_port_clr_tx_buffer(u);

    ASSERT((upath[0] != 0), "UART path not intilaized", "0")
    ASSERT((baud != 0), "UART baud rate not intilaized", "0")
    ASSERT((u != NULL), "UART: DAP_UART struct pointer in not intilaized", "0")
    if ((upath[0] == 0) || (baud == 0) || (u == NULL)){
        return DAP_DATA_INIT_ERROR;
    }
    u->baud = baud;

    // open serial port
    u->fd_uart = open(upath, DAP_UART_ACCESS_FLAGS);
    if (u->fd_uart == -1 ) {
        ASSERT(ASSERT_FAIL, "Failed to open port", strerror(errno))
        return DAP_DATA_INIT_ERROR;
    }

    // set com attributes
    if (dap_port_init_attributes(u) != DAP_SUCCESS) {
        ASSERT(ASSERT_FAIL, "Could not intialize UART attributes", strerror(errno))
        return DAP_DATA_INIT_ERROR;
    }

    tcflush(u->fd_uart, TCIFLUSH);

    return results;
}


// given a fd (descriptor) determine which DAP_UART struct to use (helper function)
static int dap_which_uart(int fd, struct DAP_UART *u1, struct DAP_UART *u2) {

    if ((u1->fd_uart <= 0) && (u2->fd_uart <= 0)){
        // both UART ports are not initialized
        ASSERT(ASSERT_FAIL, "UART: Both uarts have not been opened sucessfully", "fd_uart<=0")
        return DAP_ERROR;
    }
    if (fd == u1->fd_uart) {
        return DAP_DATA_SRC1;
    }
    else if (fd == u2->fd_uart) {
        return DAP_DATA_SRC2;
    }
    else {
        // note: a failed open will have a -1 stored in the descriptor
        // and a uart port that is unused will have a 0 stored in the file descriptor
        ASSERT(ASSERT_FAIL, "UART: Could not determine which uart has created event", "-1")
        return DAP_ERROR;
    }
}

// determine pointer to first open address in circular buffer (helper function)
static unsigned char* dap_next_addr(struct DAP_UART *u) {

    unsigned char *maxptr;
    unsigned char *ptr;

    maxptr = u->buf_rx + DAP_UART_BUF_SIZE;
    ptr = u->read_ptr + u->num_unread;

    if (ptr >= maxptr){
        // circle back, adjust ptr
        ptr = ptr - maxptr;
    }
    ASSERT(((ptr < maxptr) && (ptr >= u->buf_rx)), "UART: first open pointer out of range", "-1")

    return ptr;
}

// copy data to circular buffer (helper function)
static void dap_rx_cp (unsigned int num, unsigned char *src, struct DAP_UART *u) {

    unsigned int i;
    unsigned int index;
    unsigned char *ptr;
    unsigned char *dst;

    ASSERT((num != 0), "UART Warning: n is equal to 0, nothing to do", "-1")
    if (num == 0) {
        // nothing to do
        return;
    }
    ASSERT((src != NULL), "UART: src pointer is NULL", "-1")
    if (src == NULL) {
        return;
    }
    ASSERT((u != NULL), "UART: u pointer is NULL", "-1")
    if (u == NULL) {
        return;
    }

    ptr = dap_next_addr(u);

    index = ptr - u->buf_rx;
    ASSERT((index < DAP_UART_BUF_SIZE), "UART: index to large, seg fault possible", "-1")

    // copy data
    for (i=0; i < num; i++) {
        dst = ptr + index;
        *dst = *src;
        src++;
        index++;
        index = index % DAP_UART_BUF_SIZE;
    }

    u->num_unread += num;
    ASSERT((u->num_unread < DAP_UART_BUF_SIZE), "UART: num_unread to large, seg fault possible", "-1")
    u->read_ptr = ptr + index;
    ASSERT((index < DAP_UART_BUF_SIZE), "UART: read_ptr to large, seg fault possible", "-1")
}

// copy recieve data to uart structs (helper function)
static int dap_uart_rx_copy (int num, int fd, unsigned char *buf) {

    unsigned int src;

    src = dap_which_uart(fd, &uart1, &uart2);
    ASSERT((src != DAP_ERROR), "UART: Can not copy rx data, possible invalid fd", "-1")

    switch (src) {
        case DAP_DATA_SRC1:
            dap_rx_cp (num, buf, &uart1);
        break;

        case  DAP_DATA_SRC2:
            dap_rx_cp (num, buf, &uart2);
        break;

        default:
            // log error
            ASSERT(ASSERT_FAIL, "UART: Could not copy rx data", "rx data not copied")
        break;
    }

    // returns the dap data source that rx was copied to or error
    return src;
}

// transmit data in  buf_tx buffer
int dap_port_transmit (struct DAP_UART *u) {

    int result;

    if (u->fd_uart == 0) {
        ASSERT(ASSERT_FAIL, "UART: Transmit, port not open", strerror(errno))
        return DAP_ERROR;
    }

    if (u->num_to_tx == 0) {
        // no data to transmit
        return 0;
    }

    result = write(u->fd_uart, u->buf_tx, u->num_to_tx);
    if (result == -1) {
        ASSERT((result != -1), "UART: Could not transmit UART data", strerror(errno))
    }
    else {
        ASSERT((result == u->num_to_tx), "UART: Incomplete data write", strerror(errno))
    }

    u->num_to_tx = 0;

    // returns number of bytes transmitted or -1 if failed
    return result;

}

// TODO - Depricate in future, recieve performed by thr_uart_epoll thread
// recieve data in  buf_rx buffer
int dap_port_recieve (struct DAP_UART *u) {

    int result;

    if (u->fd_uart == 0) {
        ASSERT(ASSERT_FAIL, "Recieve: UART port not open", strerror(errno))
        return -1;
    }

    result = read(u->fd_uart, u->buf_rx, sizeof(u->buf_rx));    //TODO - size?
    if (result == -1) {
        ASSERT(ASSERT_FAIL, "Recieve: Could not recieve UART data", strerror(errno))
    }

    // returns number of bytes recieved, or 0 for EOF, or -1 if failed
    return result;

}

// create an epoll instance and add the uarts to to watch
// notes:
// * epoll requires Linux kernel 2.6 or better
// * initialzie all ports with dap_port_init prior to calling
static int dap_uart_epoll_init (struct DAP_UART_EPOLL *uep, struct DAP_UART *u1, struct DAP_UART *u2) {

    int result = DAP_SUCCESS;

    // create an epoll descriptor
    uep->epfd = epoll_create(DAP_NUM_OF_SRC);
    if (uep->epfd == -1){
        ASSERT((uep->epfd != -1), "UART EPOLL: Could not create an epoll descriptor - epoll_create", strerror(errno))
        return DAP_ERROR;
    }

    if (u1->fd_uart >= 0) {
        // add the u1 file descriptor to the list of i/o to watch
        // set interest list: unblocked read possible (EPOLLIN) and input data has been recieved (EPOLLET, edge triggered)
        uep->ev.data.fd = u1->fd_uart;
        uep->ev.events = EPOLLIN | EPOLLET;
        if (epoll_ctl(uep->epfd, EPOLL_CTL_ADD, u1->fd_uart, &uep->ev) == -1) {
            ASSERT(ASSERT_FAIL, "UART EPOLL: Could not add uart1 - epoll_ctl", strerror(errno))
            return DAP_ERROR;
        }
    }

    if (u2->fd_uart >= 0) {
        // add the u2 file descriptor to the list of i/o to watch
        // set interest list: unblocked read possible (EPOLLIN) and input data has been recieved (EPOLLET, edge triggered)
        uep->ev.data.fd = u2->fd_uart;
        uep->ev.events = EPOLLIN | EPOLLET;
        if (epoll_ctl(uep->epfd, EPOLL_CTL_ADD, u2->fd_uart, &uep->ev) == -1) {
            ASSERT(ASSERT_FAIL, "UART EPOLL: Could not add uart2 - epoll_ctl", strerror(errno))
            return DAP_ERROR;
        }
    }

    return result;
}


// uart rx thread
static void *dap_uart_epoll_thr(void *arg) {

    int ready;
    int s;
    int j;
    int src;
    unsigned char buf[DAP_UART_BUF_SIZE];

    uep.numOpenFds = DAP_NUM_OF_SRC;

    while (uep.numOpenFds > 0) {

        /* Fetch up to MAX_EVENTS items from the ready list */
        ready = epoll_wait(uep.epfd, uep.evlist, EP_MAX_EVENTS, -1);
        if (ready == -1) {
            if (errno == EINTR) {   // TODO - add more sigs ?
                // Restart if interrupted by signal
                continue;
            }
            else {
                ASSERT((ready != -1), "UART: epoll_wait error", strerror(errno))
                return DAP_ERROR;
            }
        }

        /* process returned list of events */
        for (j = 0; j < ready; j++) {

            if (uep.evlist[j].events & EPOLLIN) {
                s = read(uep.evlist[j].data.fd, buf, DAP_UART_BUF_SIZE);
                if (s == -1){
                    // read error, log, no data to read
                    ASSERT((s != -1), "UART: read error", strerror(errno))
                }
                else {
                    // store data
                    src = dap_uart_rx_copy (s, uep.evlist[j].data.fd, buf);
                    ASSERT((src != DAP_ERROR), "UART: rx data not saved", "-1")
                }
            }
            else if (uep.evlist[j].events & EPOLLHUP) {
                // Hang up event, Lost connection, log
                ASSERT(ASSERT_FAIL, "UART: Hang up, Lost UART connection", strerror(errno))
                close(uep.)
            }
            else if (uep.evlist[j].events & EPOLLERR) {
                // log epoll error
                ASSERT(ASSERT_FAIL, "UART: epoll error", strerror(errno))
            }
        }
    }
}
