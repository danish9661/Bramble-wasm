/*
 * W5500 Ethernet Controller (SPI Device Plugin)
 *
 * Emulates the WIZnet W5500 hardwired TCP/IP Ethernet controller.
 * Processes SPI frames with a 3-byte header (2 address + 1 control)
 * followed by data bytes for read or write operations.
 *
 * Block Select Byte (BSB) routes access to common registers,
 * per-socket registers, or per-socket TX/RX buffers. The W5500
 * supports 8 independent sockets, each with 2KB TX and 2KB RX
 * buffer space.
 *
 * This is a register-level model for firmware development. No actual
 * network I/O is performed -- socket commands update status registers
 * to simulate expected state transitions.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "w5500.h"

/* ========================================================================
 * BSB decoding helpers
 *
 * BSB encoding (5 bits):
 *   0        = common registers
 *   n*4 + 1  = socket n registers   (n = 0..7)
 *   n*4 + 2  = socket n TX buffer
 *   n*4 + 3  = socket n RX buffer
 * ======================================================================== */

/* Returns block type: 0=common, 1=socket reg, 2=TX buf, 3=RX buf */
static int bsb_type(uint8_t bsb) {
    if (bsb == 0) return 0;
    return ((bsb - 1) & 3) + 1;  /* 1=reg, 2=TX, 3=RX */
}

/* Returns socket number for non-common BSBs (0-7) */
static int bsb_socket(uint8_t bsb) {
    if (bsb == 0) return -1;
    return (bsb - 1) >> 2;
}

/* ========================================================================
 * Socket command processing
 * ======================================================================== */

static void set_sock_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Build a sockaddr_in from socket registers */
static void w5500_build_addr(w5500_socket_t *s, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    /* Dest IP from regs (big-endian on wire) */
    uint32_t ip = ((uint32_t)s->regs[W5500_Sn_DIPR0] << 24) |
                  ((uint32_t)s->regs[W5500_Sn_DIPR0 + 1] << 16) |
                  ((uint32_t)s->regs[W5500_Sn_DIPR0 + 2] << 8) |
                  (uint32_t)s->regs[W5500_Sn_DIPR0 + 3];
    addr->sin_addr.s_addr = htonl(ip);
    /* Dest port */
    uint16_t port = ((uint16_t)s->regs[W5500_Sn_DPORT0] << 8) |
                    s->regs[W5500_Sn_DPORT0 + 1];
    addr->sin_port = htons(port);
}

/* Close any host socket associated with this W5500 socket */
static void w5500_close_host_sock(w5500_socket_t *s) {
    if (s->host_fd >= 0) {
        close(s->host_fd);
        s->host_fd = -1;
    }
    if (s->host_listen_fd >= 0) {
        close(s->host_listen_fd);
        s->host_listen_fd = -1;
    }
}

static void w5500_process_socket_cmd(w5500_t *dev, int sock) {
    w5500_socket_t *s = &dev->sockets[sock];
    uint8_t cmd = s->regs[W5500_Sn_CR];
    uint8_t mode = s->regs[W5500_Sn_MR];

    if (cmd == 0) return;

    switch (cmd) {
    case W5500_CMD_OPEN:
        if ((mode & 0x0F) == W5500_MR_TCP) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_INIT;
            if (dev->live) {
                w5500_close_host_sock(s);
                s->host_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (s->host_fd >= 0) set_sock_nonblock(s->host_fd);
            }
        } else if ((mode & 0x0F) == W5500_MR_UDP) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_UDP;
            if (dev->live) {
                w5500_close_host_sock(s);
                s->host_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (s->host_fd >= 0) {
                    set_sock_nonblock(s->host_fd);
                    /* Bind to source port if set */
                    uint16_t src_port = ((uint16_t)s->regs[W5500_Sn_PORT0] << 8) |
                                        s->regs[W5500_Sn_PORT0 + 1];
                    if (src_port > 0) {
                        struct sockaddr_in bind_addr;
                        memset(&bind_addr, 0, sizeof(bind_addr));
                        bind_addr.sin_family = AF_INET;
                        bind_addr.sin_addr.s_addr = INADDR_ANY;
                        bind_addr.sin_port = htons(src_port);
                        int opt = 1;
                        setsockopt(s->host_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                        bind(s->host_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
                    }
                }
            }
        } else if ((mode & 0x0F) == W5500_MR_MACRAW) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_MACRAW;
        }
        /* TX free = full buffer size */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        break;

    case W5500_CMD_LISTEN:
        if (s->regs[W5500_Sn_SR] == W5500_SOCK_INIT) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_LISTEN;
            if (dev->live && s->host_fd >= 0) {
                uint16_t src_port = ((uint16_t)s->regs[W5500_Sn_PORT0] << 8) |
                                    s->regs[W5500_Sn_PORT0 + 1];
                struct sockaddr_in bind_addr;
                memset(&bind_addr, 0, sizeof(bind_addr));
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_addr.s_addr = INADDR_ANY;
                bind_addr.sin_port = htons(src_port);
                int opt = 1;
                setsockopt(s->host_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                if (bind(s->host_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0 &&
                    listen(s->host_fd, 1) == 0) {
                    s->host_listen_fd = s->host_fd;
                    s->host_fd = -1;
                }
            }
        }
        break;

    case W5500_CMD_CONNECT:
        if (s->regs[W5500_Sn_SR] == W5500_SOCK_INIT) {
            s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
            if (dev->live && s->host_fd >= 0) {
                struct sockaddr_in dest;
                w5500_build_addr(s, &dest);
                /* Non-blocking connect — may succeed or be in progress */
                int rc = connect(s->host_fd, (struct sockaddr *)&dest, sizeof(dest));
                if (rc < 0 && errno != EINPROGRESS) {
                    fprintf(stderr, "[W5500] Socket %d connect failed: %s\n",
                            sock, strerror(errno));
                }
            }
        }
        break;

    case W5500_CMD_CLOSE:
        s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        if (dev->live) w5500_close_host_sock(s);
        break;

    case W5500_CMD_SEND: {
        /* Read TX data from buffer */
        uint16_t tx_rd = ((uint16_t)s->regs[W5500_Sn_TX_RD0] << 8) |
                         s->regs[W5500_Sn_TX_RD0 + 1];
        uint16_t tx_wr = ((uint16_t)s->regs[W5500_Sn_TX_WR0] << 8) |
                         s->regs[W5500_Sn_TX_WR0 + 1];
        uint16_t data_len = tx_wr - tx_rd;

        if (dev->live && s->host_fd >= 0 && data_len > 0) {
            uint8_t send_buf[W5500_TX_BUF_SIZE];
            for (uint16_t i = 0; i < data_len; i++) {
                send_buf[i] = s->tx_buf[(tx_rd + i) % W5500_TX_BUF_SIZE];
            }

            uint8_t mode_val = s->regs[W5500_Sn_MR] & 0x0F;
            if (mode_val == W5500_MR_UDP) {
                struct sockaddr_in dest;
                w5500_build_addr(s, &dest);
                sendto(s->host_fd, send_buf, data_len, 0,
                       (struct sockaddr *)&dest, sizeof(dest));
            } else {
                send(s->host_fd, send_buf, data_len, MSG_NOSIGNAL);
            }
        }

        /* Advance TX read pointer to write pointer */
        s->regs[W5500_Sn_TX_RD0] = s->regs[W5500_Sn_TX_WR0];
        s->regs[W5500_Sn_TX_RD0 + 1] = s->regs[W5500_Sn_TX_WR0 + 1];
        /* Restore full TX free space */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        /* Set SEND_OK interrupt */
        s->regs[W5500_Sn_IR] |= 0x10;
        break;
    }

    case W5500_CMD_RECV:
        /* Advance RX read pointer, clear received size */
        s->regs[W5500_Sn_RX_RSR0] = 0;
        s->regs[W5500_Sn_RX_RSR0 + 1] = 0;
        break;

    default:
        break;
    }

    /* Command register auto-clears after execution */
    s->regs[W5500_Sn_CR] = 0x00;
}

/* ========================================================================
 * Read/write a single byte from the appropriate block
 * ======================================================================== */

static uint8_t w5500_read_byte(w5500_t *dev, uint8_t bsb, uint16_t addr) {
    int type = bsb_type(bsb);
    int sock = bsb_socket(bsb);

    switch (type) {
    case 0: /* Common registers */
        if (addr < W5500_COMMON_REG_SIZE)
            return dev->common[addr];
        return 0x00;

    case 1: /* Socket registers */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS &&
            addr < W5500_SOCKET_REG_SIZE)
            return dev->sockets[sock].regs[addr];
        return 0x00;

    case 2: /* Socket TX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS)
            return dev->sockets[sock].tx_buf[addr % W5500_TX_BUF_SIZE];
        return 0x00;

    case 3: /* Socket RX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS)
            return dev->sockets[sock].rx_buf[addr % W5500_RX_BUF_SIZE];
        return 0x00;
    }

    return 0x00;
}

static void w5500_write_byte(w5500_t *dev, uint8_t bsb, uint16_t addr,
                             uint8_t val) {
    int type = bsb_type(bsb);
    int sock = bsb_socket(bsb);

    switch (type) {
    case 0: /* Common registers */
        if (addr == W5500_VERSIONR) return;  /* Read-only */
        if (addr < W5500_COMMON_REG_SIZE)
            dev->common[addr] = val;
        break;

    case 1: /* Socket registers */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS &&
            addr < W5500_SOCKET_REG_SIZE) {
            dev->sockets[sock].regs[addr] = val;
            /* Process command register writes */
            if (addr == W5500_Sn_CR)
                w5500_process_socket_cmd(dev, sock);
        }
        break;

    case 2: /* Socket TX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS)
            dev->sockets[sock].tx_buf[addr % W5500_TX_BUF_SIZE] = val;
        break;

    case 3: /* Socket RX buffer */
        if (sock >= 0 && sock < W5500_NUM_SOCKETS)
            dev->sockets[sock].rx_buf[addr % W5500_RX_BUF_SIZE] = val;
        break;
    }
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

void w5500_init(w5500_t *dev) {
    memset(dev, 0, sizeof(*dev));

    /* Version register (read-only) */
    dev->common[W5500_VERSIONR] = 0x04;

    /* Default MAC: 02:00:00:00:00:01 */
    dev->common[W5500_SHAR0]     = 0x02;
    dev->common[W5500_SHAR0 + 1] = 0x00;
    dev->common[W5500_SHAR0 + 2] = 0x00;
    dev->common[W5500_SHAR0 + 3] = 0x00;
    dev->common[W5500_SHAR0 + 4] = 0x00;
    dev->common[W5500_SHAR0 + 5] = 0x01;

    /* IP defaults to 0.0.0.0 (already zero from memset) */

    /* PHY config: link up, 100Mbps full-duplex, not in reset */
    dev->common[W5500_PHYCFGR] = W5500_PHY_RST | W5500_PHY_LINK |
                                  W5500_PHY_SPD | W5500_PHY_DPX;

    /* Default retry time: 200ms (0x07D0 = 2000 * 100us) */
    dev->common[W5500_RTR0]     = 0x07;
    dev->common[W5500_RTR0 + 1] = 0xD0;

    /* Default retry count */
    dev->common[W5500_RCR] = 0x08;

    /* Live networking defaults */
    dev->live = 0;
    dev->vnet_port = -1;

    /* Initialize each socket with default buffer sizes and TTL */
    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        w5500_socket_t *s = &dev->sockets[i];
        s->regs[W5500_Sn_RXBUF_SIZE] = 2;  /* 2KB */
        s->regs[W5500_Sn_TXBUF_SIZE] = 2;  /* 2KB */
        s->regs[W5500_Sn_TTL] = 128;
        s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        /* TX free = full buffer */
        s->regs[W5500_Sn_TX_FSR0] = (W5500_TX_BUF_SIZE >> 8) & 0xFF;
        s->regs[W5500_Sn_TX_FSR0 + 1] = W5500_TX_BUF_SIZE & 0xFF;
        s->host_fd = -1;
        s->host_listen_fd = -1;
    }
}

/* ========================================================================
 * SPI interface callbacks
 * ======================================================================== */

uint8_t w5500_spi_xfer(void *ctx, uint8_t mosi) {
    w5500_t *dev = (w5500_t *)ctx;
    uint8_t miso = 0xFF;

    if (!dev->cs_active) return 0xFF;

    switch (dev->phase) {
    case W5500_PHASE_ADDR_HI:
        dev->addr = (uint16_t)mosi << 8;
        dev->phase = W5500_PHASE_ADDR_LO;
        break;

    case W5500_PHASE_ADDR_LO:
        dev->addr |= mosi;
        dev->phase = W5500_PHASE_CONTROL;
        break;

    case W5500_PHASE_CONTROL:
        dev->bsb = (mosi >> 3) & 0x1F;
        dev->rw  = (mosi >> 2) & 1;
        dev->phase = W5500_PHASE_DATA;
        break;

    case W5500_PHASE_DATA:
        if (dev->rw) {
            /* Write */
            w5500_write_byte(dev, dev->bsb, dev->addr, mosi);
        } else {
            /* Read */
            miso = w5500_read_byte(dev, dev->bsb, dev->addr);
        }
        dev->addr++;
        break;
    }

    return miso;
}

void w5500_spi_cs(void *ctx, int cs_active) {
    w5500_t *dev = (w5500_t *)ctx;
    dev->cs_active = cs_active;

    if (cs_active) {
        /* CS asserted: reset frame state machine */
        dev->phase = W5500_PHASE_ADDR_HI;
    }
}

/* ========================================================================
 * Live Networking: Poll host sockets for incoming data
 * ======================================================================== */

void w5500_poll(w5500_t *dev) {
    if (!dev->live) return;

    for (int i = 0; i < W5500_NUM_SOCKETS; i++) {
        w5500_socket_t *s = &dev->sockets[i];

        /* Accept incoming TCP connections */
        if (s->host_listen_fd >= 0 &&
            s->regs[W5500_Sn_SR] == W5500_SOCK_LISTEN) {
            struct pollfd pfd = { .fd = s->host_listen_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in client;
                socklen_t clen = sizeof(client);
                int cfd = accept(s->host_listen_fd, (struct sockaddr *)&client, &clen);
                if (cfd >= 0) {
                    set_sock_nonblock(cfd);
                    s->host_fd = cfd;
                    s->regs[W5500_Sn_SR] = W5500_SOCK_ESTABLISHED;
                    /* Store client IP/port in dest registers */
                    uint32_t ip = ntohl(client.sin_addr.s_addr);
                    s->regs[W5500_Sn_DIPR0]     = (ip >> 24) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 1] = (ip >> 16) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 2] = (ip >>  8) & 0xFF;
                    s->regs[W5500_Sn_DIPR0 + 3] = ip & 0xFF;
                    uint16_t port = ntohs(client.sin_port);
                    s->regs[W5500_Sn_DPORT0]     = (port >> 8) & 0xFF;
                    s->regs[W5500_Sn_DPORT0 + 1] = port & 0xFF;
                    /* Set CON interrupt */
                    s->regs[W5500_Sn_IR] |= 0x01;
                }
            }
        }

        /* Read incoming data into RX buffer */
        if (s->host_fd >= 0 &&
            (s->regs[W5500_Sn_SR] == W5500_SOCK_ESTABLISHED ||
             s->regs[W5500_Sn_SR] == W5500_SOCK_UDP)) {

            uint16_t rx_rsr = ((uint16_t)s->regs[W5500_Sn_RX_RSR0] << 8) |
                              s->regs[W5500_Sn_RX_RSR0 + 1];
            uint16_t free_space = W5500_RX_BUF_SIZE - rx_rsr;
            if (free_space == 0) continue;

            struct pollfd pfd = { .fd = s->host_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint16_t rx_wr = ((uint16_t)s->regs[W5500_Sn_RX_WR0] << 8) |
                                 s->regs[W5500_Sn_RX_WR0 + 1];

                uint8_t tmp[W5500_RX_BUF_SIZE];
                ssize_t n = recv(s->host_fd, tmp,
                                free_space < sizeof(tmp) ? free_space : sizeof(tmp), 0);
                if (n > 0) {
                    for (ssize_t j = 0; j < n; j++) {
                        s->rx_buf[(rx_wr + (uint16_t)j) % W5500_RX_BUF_SIZE] = tmp[j];
                    }
                    rx_wr = (uint16_t)(rx_wr + (uint16_t)n);
                    s->regs[W5500_Sn_RX_WR0]     = (rx_wr >> 8) & 0xFF;
                    s->regs[W5500_Sn_RX_WR0 + 1] = rx_wr & 0xFF;
                    rx_rsr = (uint16_t)(rx_rsr + (uint16_t)n);
                    s->regs[W5500_Sn_RX_RSR0]     = (rx_rsr >> 8) & 0xFF;
                    s->regs[W5500_Sn_RX_RSR0 + 1] = rx_rsr & 0xFF;
                    /* Set RECV interrupt */
                    s->regs[W5500_Sn_IR] |= 0x04;
                } else if (n == 0) {
                    /* Peer disconnected */
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                    s->regs[W5500_Sn_IR] |= 0x02;  /* DISCON interrupt */
                    close(s->host_fd);
                    s->host_fd = -1;
                }
            }

            /* Check for errors/disconnect */
            if (s->host_fd >= 0) {
                struct pollfd pfd2 = { .fd = s->host_fd, .events = 0 };
                if (poll(&pfd2, 1, 0) > 0 &&
                    (pfd2.revents & (POLLERR | POLLHUP))) {
                    s->regs[W5500_Sn_SR] = W5500_SOCK_CLOSED;
                    s->regs[W5500_Sn_IR] |= 0x02;
                    close(s->host_fd);
                    s->host_fd = -1;
                }
            }
        }
    }
}

void w5500_set_live(w5500_t *dev, int enable) {
    dev->live = enable;
    if (enable) {
        fprintf(stderr, "[W5500] Live networking enabled\n");
    }
}
