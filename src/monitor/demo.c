#include "monitor/monitoringSys.h"
/*
 * demo (M2) - a timer heartbeat and a TCP server share ONE select() loop.
 *
 * Try it:   ./demo        then in another shell:  telnet localhost 9000
 * You'll see heartbeats keep ticking while your typed lines are echoed
 * back by the accept/read callbacks - all in a single thread.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000

/* --- recurring timer: proves the loop stays live during I/O waits --- */
static int heartbeat_cb(void *arg[]) {
    int *n = (int *)arg[0];
    printf("  [heartbeat] tick #%d\n", ++(*n));
    fflush(stdout);
    return 0;
}

/* --- fires when a connected client's fd is readable --- */
static int client_cb(void *arg[]) {
    void **slot  = (void **)arg[0];  /* slot[0]=fd, slot[1]=this fd's handle */
    int   fd     = (int)(long)slot[0];
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {                   /* peer closed / error -> tear down */
        printf("  [client %d] disconnected\n", fd);
        sys_mon_aio_remove(slot[1]);
        close(fd);
        fflush(stdout);
        return 0;
    }
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    printf("  [client %d] received: \"%s\"\n", fd, buf);
    dprintf(fd, "echo: %s\n", buf);  /* echo back over TCP */
    fflush(stdout);
    return 0;
}

/* per-connection slot storage (kept alive for the connection's lifetime) */
static void *g_client_arg[8][2];
static int   g_client_slots = 0;

/* --- fires when the listening socket is readable: a client is waiting --- */
static int accept_cb(void *arg[]) {
    int listen_fd = (int)(long)arg[0];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int cfd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        SM_LOG("accept failed: %s", strerror(errno));
        return 0;
    }
    printf("  [server] accepted connection from %s (fd %d)\n",
           inet_ntoa(peer.sin_addr), cfd);
    fflush(stdout);

    if (g_client_slots >= 8) {       /* demo cap */
        close(cfd);
        return 0;
    }
    /* slot survives the callback; pass its address so client_cb can read
     * both the fd and (after add returns) its own handle for self-removal */
    void **slot = g_client_arg[g_client_slots++];
    slot[0] = (void *)(long)cfd;
    SysMonCallback_t ccb = { .cb_name = "client", .cb_callback = client_cb };
    ccb.cb_arg[0] = slot;
    slot[1] = sys_mon_aio_add(&ccb, cfd);
    return 0;
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { SM_LOG("socket: %s", strerror(errno)); return -1; }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SM_LOG("bind: %s", strerror(errno)); close(fd); return -1;
    }
    if (listen(fd, 5) < 0) {
        SM_LOG("listen: %s", strerror(errno)); close(fd); return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    sys_mon_init_t smit = { .smi_name = "demo_mon", .smi_cli_port = PORT };
    void *h = sys_mon_init(&smit, argc, argv);
    if (!h) { return 1; }

    static int count = 0;
    SysMonCallback_t hb = { .cb_name = "hb", .cb_callback = heartbeat_cb };
    hb.cb_arg[0] = &count;
    sys_mon_timer_set(&hb, SM_TIMER_RECURE, 1000);

    int lfd = make_listener();
    if (lfd < 0) { return 1; }
    static SysMonCallback_t acc = { .cb_name = "accept", .cb_callback = accept_cb };
    acc.cb_arg[0] = (void *)(long)lfd;
    sys_mon_aio_add(&acc, lfd);

    printf("running: heartbeat every 1s + TCP server on localhost:%d\n", PORT);
    printf("connect with:  telnet localhost %d   (Ctrl-C here to stop)\n", PORT);
    return sys_mon_run(h);
}
