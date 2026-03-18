#include <stdio.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

// Shared TCP command buffer in main.c
#define RX_LINE_MAX 32
extern volatile char g_tcp_cmd[RX_LINE_MAX];
extern volatile int  g_tcp_cmd_updated;

int transfer_data() {
    return 0; // RX handled in callback
}

void print_app_header()
{
    xil_printf("\n\r----- TCP tracking server -----\n\r");
    xil_printf("Send newline-terminated commands to port 6000:\n\r");
    xil_printf("  left\\n  right\\n  none\\n\n\r");
}

// ---- Line buffering ----
static char linebuf[RX_LINE_MAX];
static int  idx = 0;

static void reset_line(void) {
    idx = 0;
    linebuf[0] = '\0';
}

static void normalize_and_store(const char *s)
{
    // trim leading spaces
    while (*s == ' ' || *s == '\t') s++;

    // copy + lowercase + trim trailing spaces (within RX_LINE_MAX)
    char tmp[RX_LINE_MAX];
    int n = 0;
    while (*s && n < RX_LINE_MAX - 1) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tmp[n++] = c;
    }
    tmp[n] = '\0';

    // trim trailing whitespace
    while (n > 0 && (tmp[n-1] == ' ' || tmp[n-1] == '\t')) {
        tmp[--n] = '\0';
    }

    // empty line -> "none"
    if (n == 0) {
        strcpy(tmp, "none");
    }

    // store into global volatile buffer
    for (int i = 0; i < RX_LINE_MAX; i++) {
        g_tcp_cmd[i] = tmp[i];
        if (tmp[i] == '\0') break;
    }
    g_tcp_cmd[RX_LINE_MAX - 1] = '\0';
    g_tcp_cmd_updated = 1;
}

static err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                           struct pbuf *p, err_t err)
{
    (void)arg;

    if (!p) {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        reset_line();
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const char *d = (const char*)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            char c = d[i];

            if (c == '\r') continue;

            if (c == '\n') {
                linebuf[idx] = '\0';
                normalize_and_store(linebuf);   // <-- update tracking command
                reset_line();
                continue;
            }

            if (idx < RX_LINE_MAX - 1) {
                linebuf[idx++] = c;
            } else {
                reset_line(); // overflow reset
            }
        }
    }

    pbuf_free(p);

    // Optional ACK
    const char ok[] = "OK\n";
    tcp_write(tpcb, ok, sizeof(ok) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);

    return ERR_OK;
}

static err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    static int connection = 1;
    (void)arg;

    if (err != ERR_OK || !newpcb) return ERR_VAL;

    tcp_recv(newpcb, recv_callback);
    tcp_arg(newpcb, (void*)(UINTPTR)connection);
    connection++;

    reset_line();

    const char banner[] = "Zybo connected\n";
    tcp_write(newpcb, banner, sizeof(banner) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);

    return ERR_OK;
}

int start_application()
{
    struct tcp_pcb *pcb;
    err_t err;
    unsigned port = 6000;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("Error creating PCB. Out of Memory\n\r");
        return -1;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        xil_printf("Unable to bind to port %d: err = %d\n\r", port, err);
        return -2;
    }

    tcp_arg(pcb, NULL);

    pcb = tcp_listen(pcb);
    if (!pcb) {
        xil_printf("Out of memory while tcp_listen\n\r");
        return -3;
    }

    tcp_accept(pcb, accept_callback);

    xil_printf("TCP tracking server started @ port %d\n\r", port);
    return 0;
}
