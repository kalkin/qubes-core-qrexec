/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <assert.h>
#include "qrexec.h"
#include "libqrexec-utils.h"

// whether qrexec-client should replace problematic bytes with _ before printing the output
int replace_chars_stdout = 0;
int replace_chars_stderr = 0;

#define VCHAN_BUFFER_SIZE 65536

#define QREXEC_DATA_MIN_VERSION QREXEC_PROTOCOL_V2

int local_stdin_fd, local_stdout_fd;
pid_t local_pid = 0;
/* flag if this is "remote" end of service call. In this case swap STDIN/STDOUT
 * msg types and send exit code at the end */
int is_service = 0;
int child_exited = 0;

extern char **environ;

static char *xstrdup(const char *arg) {
    char *retval = strdup(arg);
    if (!retval) {
        fputs("Out of memory in xstrdup()\n", stderr);
        abort();
    }
    return retval;
}

static void set_remote_domain(const char *src_domain_name) {
    if (setenv("QREXEC_REMOTE_DOMAIN", src_domain_name, 1)) {
        fputs("Cannot set QREXEC_REMOTE_DOMAIN\n", stderr);
        abort();
    }
}

static void close_stdin_fd(void) {
    if (local_stdin_fd < 0)
        return;
    if (shutdown(local_stdin_fd, SHUT_WR) && errno != ENOTSOCK) {
        fputs("Cannot shutdown socket\n", stderr);
        abort();
    }
    if (local_stdin_fd != local_stdout_fd) {
        /* restore flags, as we may have not the only copy of this file descriptor */
        set_block(local_stdin_fd);
        if (close(local_stdin_fd)) {
            fputs("Cannot close socket\n", stderr);
            abort();
        }
    }
    local_stdin_fd = -1;
}

static void close_stdout_fd(void) {
    if (local_stdout_fd < 0)
        return;
    if (shutdown(local_stdout_fd, SHUT_RD) && errno != ENOTSOCK) {
        fputs("Cannot shutdown socket\n", stderr);
        abort();
    }
    if (local_stdout_fd != local_stdin_fd) {
        set_block(local_stdout_fd);
        if (close(local_stdout_fd)) {
            fputs("Cannot close socket\n", stderr);
            abort();
        }
    }
    local_stdout_fd = -1;
}

/* initialize data_protocol_version */
static int handle_agent_handshake(libvchan_t *vchan, int remote_send_first)
{
    struct msg_header hdr;
    struct peer_info info;
    int data_protocol_version = -1;
    int who = 0; // even - send to remote, odd - receive from remote

    while (who < 2) {
        if ((who+remote_send_first) & 1) {
            if (!read_vchan_all(vchan, &hdr, sizeof(hdr))) {
                perror("daemon handshake");
                return -1;
            }
            if (hdr.type != MSG_HELLO || hdr.len != sizeof(info)) {
                fprintf(stderr, "Invalid daemon MSG_HELLO\n");
                return -1;
            }
            if (!read_vchan_all(vchan, &info, sizeof(info))) {
                perror("daemon handshake");
                return -1;
            }

            data_protocol_version = info.version < QREXEC_PROTOCOL_VERSION ?
                                    info.version : QREXEC_PROTOCOL_VERSION;
            if (data_protocol_version < QREXEC_DATA_MIN_VERSION) {
                fprintf(stderr, "Incompatible daemon protocol version "
                        "(daemon %d, client %d)\n",
                        info.version, QREXEC_PROTOCOL_VERSION);
                return -1;
            }
        } else {
            hdr.type = MSG_HELLO;
            hdr.len = sizeof(info);
            info.version = QREXEC_PROTOCOL_VERSION;

            if (!write_vchan_all(vchan, &hdr, sizeof(hdr))) {
                fprintf(stderr, "Failed to send MSG_HELLO hdr to daemon\n");
                return -1;
            }
            if (!write_vchan_all(vchan, &info, sizeof(info))) {
                fprintf(stderr, "Failed to send MSG_HELLO to daemon\n");
                return -1;
            }
        }
        who++;
    }
    return data_protocol_version;
}

static int handle_daemon_handshake(int fd)
{
    struct msg_header hdr;
    struct peer_info info;

    /* daemon send MSG_HELLO first */
    if (!read_all(fd, &hdr, sizeof(hdr))) {
        perror("daemon handshake");
        return -1;
    }
    if (hdr.type != MSG_HELLO || hdr.len != sizeof(info)) {
        fprintf(stderr, "Invalid daemon MSG_HELLO\n");
        return -1;
    }
    if (!read_all(fd, &info, sizeof(info))) {
        perror("daemon handshake");
        return -1;
    }

    if (info.version != QREXEC_PROTOCOL_VERSION) {
        fprintf(stderr, "Incompatible daemon protocol version "
                "(daemon %d, client %d)\n",
                info.version, QREXEC_PROTOCOL_VERSION);
        return -1;
    }

    hdr.type = MSG_HELLO;
    hdr.len = sizeof(info);
    info.version = QREXEC_PROTOCOL_VERSION;

    if (!write_all(fd, &hdr, sizeof(hdr))) {
        fprintf(stderr, "Failed to send MSG_HELLO hdr to daemon\n");
        return -1;
    }
    if (!write_all(fd, &info, sizeof(info))) {
        fprintf(stderr, "Failed to send MSG_HELLO to daemon\n");
        return -1;
    }
    return 0;
}

static int connect_unix_socket(const char *domname)
{
    int s, len;
    struct sockaddr_un remote;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }

    remote.sun_family = AF_UNIX;
    snprintf(remote.sun_path, sizeof remote.sun_path,
            QREXEC_DAEMON_SOCKET_DIR "/qrexec.%s", domname);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if (connect(s, (struct sockaddr *) &remote, len) == -1) {
        perror("connect");
        exit(1);
    }
    if (handle_daemon_handshake(s) < 0)
        exit(1);
    return s;
}

static void sigchld_handler(int x __attribute__((__unused__)))
{
    child_exited = 1;
    signal(SIGCHLD, sigchld_handler);
}

/* called from do_fork_exec */
static _Noreturn void do_exec(char *prog, const char *username __attribute__((unused)))
{
    /* avoid calling qubes-rpc-multiplexer through shell */
    exec_qubes_rpc_if_requested(prog, environ);

    /* if above haven't executed qubes-rpc-multiplexer, pass it to shell */
    execl("/bin/bash", "bash", "-c", prog, NULL);
    perror("exec bash");
    exit(1);
}

static void do_exit(int code)
{
    int status;
    close_stdin_fd();
    close_stdout_fd();
    // sever communication lines; wait for child, if any
    // so that qrexec-daemon can count (recursively) spawned processes correctly
    waitpid(-1, &status, 0);
    exit(code);
}


static void prepare_local_fds(char *cmdline, struct buffer *stdin_buffer)
{
    if (stdin_buffer == NULL)
        abort();
    if (!cmdline) {
        local_stdin_fd = 1;
        local_stdout_fd = 0;
        return;
    }
    signal(SIGCHLD, sigchld_handler);
    execute_qubes_rpc_command(cmdline, &local_pid, &local_stdin_fd, &local_stdout_fd,
            NULL, false, stdin_buffer);
}

/* ask the daemon to allocate vchan port */
static void negotiate_connection_params(int s, int other_domid, unsigned type,
        void *cmdline_param, int cmdline_size,
        int *data_domain, int *data_port)
{
    struct msg_header hdr;
    struct exec_params params;
    hdr.type = type;
    hdr.len = sizeof(params) + cmdline_size;
    params.connect_domain = other_domid;
    params.connect_port = 0;
    if (!write_all(s, &hdr, sizeof(hdr))
            || !write_all(s, &params, sizeof(params))
            || !write_all(s, cmdline_param, cmdline_size)) {
        perror("write daemon");
        do_exit(1);
    }
    /* the daemon will respond with the same message with connect_port filled
     * and empty cmdline */
    if (!read_all(s, &hdr, sizeof(hdr))) {
        perror("read daemon");
        do_exit(1);
    }
    assert(hdr.type == type);
    if (hdr.len != sizeof(params)) {
        fprintf(stderr, "Invalid response for 0x%x\n", type);
        do_exit(1);
    }
    if (!read_all(s, &params, sizeof(params))) {
        perror("read daemon");
        do_exit(1);
    }
    *data_port = params.connect_port;
    *data_domain = params.connect_domain;
}

static void send_service_connect(int s, char *conn_ident,
        int connect_domain, int connect_port)
{
    struct msg_header hdr;
    struct exec_params exec_params;
    struct service_params srv_params;

    hdr.type = MSG_SERVICE_CONNECT;
    hdr.len = sizeof(exec_params) + sizeof(srv_params);

    exec_params.connect_domain = connect_domain;
    exec_params.connect_port = connect_port;
    strncpy(srv_params.ident, conn_ident, sizeof(srv_params.ident) - 1);
    srv_params.ident[sizeof(srv_params.ident) - 1] = '\0';

    if (!write_all(s, &hdr, sizeof(hdr))
            || !write_all(s, &exec_params, sizeof(exec_params))
            || !write_all(s, &srv_params, sizeof(srv_params))) {
        perror("write daemon");
        do_exit(1);
    }
}

static void send_exit_code(libvchan_t *vchan, int status)
{
    struct msg_header hdr;

    hdr.type = MSG_DATA_EXIT_CODE;
    hdr.len = sizeof(int);
    if (libvchan_send(vchan, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "Failed to write exit code to the agent\n");
        do_exit(1);
    }
    if (libvchan_send(vchan, &status, sizeof(status)) != sizeof(status)) {
        fprintf(stderr, "Failed to write exit code(2) to the agent\n");
        do_exit(1);
    }
}

static void handle_input(libvchan_t *vchan, int data_protocol_version)
{
    const size_t data_chunk_size = max_data_chunk_size(data_protocol_version);
    char *buf;
    int ret;
    size_t max_len;
    struct msg_header hdr;

    max_len = libvchan_buffer_space(vchan);
    if (max_len < sizeof(hdr))
        return;
    max_len -= sizeof(hdr);
    if (max_len > data_chunk_size)
        max_len = data_chunk_size;
    if (max_len == 0)
        return;

    buf = malloc(max_len);
    if (!buf) {
        fprintf(stderr, "Out of memory\n");
        do_exit(1);
    }

    ret = read(local_stdout_fd, buf, max_len);
    if (ret < 0) {
        perror("read");
        do_exit(1);
    }
    hdr.type = is_service ? MSG_DATA_STDOUT : MSG_DATA_STDIN;
    hdr.len = ret;
    if (libvchan_send(vchan, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "Failed to write STDIN data to the agent\n");
        do_exit(1);
    }
    if (ret == 0) {
        close_stdout_fd();
        if (local_stdin_fd == -1) {
            // if not a remote end of service call, wait for exit status
            if (is_service) {
                // if pipe in opposite direction already closed, no need to stay alive
                if (local_pid == 0) {
                    /* if this is "remote" service end and no real local process
                     * exists (using own stdin/out) send also fake exit code */
                    send_exit_code(vchan, 0);
                    do_exit(0);
                }
            }
        }
    }
    if (!write_vchan_all(vchan, buf, ret)) {
        if (!libvchan_is_open(vchan)) {
            // agent disconnected its end of socket, so no future data will be
            // send there; there is no sense to read from child stdout
            //
            // since vchan socket is buffered it doesn't mean all data was
            // received from the agent
            close_stdout_fd();
            if (local_stdin_fd == -1) {
                // since child does no longer accept data on its stdin, doesn't
                // make sense to process the data from the daemon
                //
                // we don't know real exit VM process code (exiting here, before
                // MSG_DATA_EXIT_CODE message)
                do_exit(1);
            }
        } else
            perror("write agent");
    }

    free(buf);
}

void do_replace_chars(char *buf, int len) {
	int i;
	unsigned char c;

	for (i = 0; i < len; i++) {
		c = buf[i];
		if ((c < '\040' || c > '\176') &&  /* not printable ASCII */
		    (c != '\t') &&                 /* not tab */
		    (c != '\n') &&                 /* not newline */
		    (c != '\r') &&                 /* not return */
		    (c != '\b') &&                 /* not backspace */
		    (c != '\a'))                   /* not bell */
			buf[i] = '_';
	}
}

static int handle_vchan_data(libvchan_t *vchan, struct buffer *stdin_buf,
        int data_protocol_version)
{
    int status;
    struct msg_header hdr;
    const size_t buf_len = max_data_chunk_size(data_protocol_version);
    char *buf;

    if (local_stdin_fd != -1) {
        switch(flush_client_data(local_stdin_fd, stdin_buf)) {
            case WRITE_STDIN_ERROR:
                perror("write stdin");
                close_stdin_fd();
                break;
            case WRITE_STDIN_BUFFERED:
                return WRITE_STDIN_BUFFERED;
            case WRITE_STDIN_OK:
                break;
        }
    }

    buf = malloc(buf_len);
    if (!buf) {
        fprintf(stderr, "Out of memory\n");
        do_exit(1);
    }

    if (libvchan_recv(vchan, &hdr, sizeof hdr) < 0) {
        perror("read vchan");
        do_exit(1);
    }
    if (hdr.len > buf_len) {
        fprintf(stderr, "client_header.len=%d\n", hdr.len);
        do_exit(1);
    }
    if (!read_vchan_all(vchan, buf, hdr.len)) {
        perror("read daemon");
        do_exit(1);
    }

    switch (hdr.type) {
        /* both directions because we can serve as either end of service call */
        case MSG_DATA_STDIN:
        case MSG_DATA_STDOUT:
            if (local_stdin_fd == -1)
                break;
            if (replace_chars_stdout)
                do_replace_chars(buf, hdr.len);
            if (hdr.len == 0) {
                close_stdin_fd();
            } else {
                switch (write_stdin(local_stdin_fd, buf, hdr.len, stdin_buf)) {
                    case WRITE_STDIN_BUFFERED:
                        free(buf);
                        return WRITE_STDIN_BUFFERED;
                    case WRITE_STDIN_ERROR:
                        if (errno == EPIPE) {
                            // local process have closed its stdin, handle data in oposite
                            // direction (if any) before exit
                            close_stdin_fd();
                        } else {
                            perror("write local stdout");
                            do_exit(1);
                        }
                        break;
                    case WRITE_STDIN_OK:
                        break;
                }
            }
            break;
        case MSG_DATA_STDERR:
            if (replace_chars_stderr)
                do_replace_chars(buf, hdr.len);
            write_all(2, buf, hdr.len);
            break;
        case MSG_DATA_EXIT_CODE:
            libvchan_close(vchan);
            if (hdr.len < sizeof(status))
                status = 255;
            else
                memcpy(&status, buf, sizeof(status));

            flush_client_data(local_stdin_fd, stdin_buf);
            do_exit(status);
            break;
        default:
            fprintf(stderr, "unknown msg %d\n", hdr.type);
            do_exit(1);
    }

    free(buf);
    /* intentionally do not distinguish between _ERROR and _OK, because in case
     * of write error, we simply eat the data - no way to report it to the
     * other side */
    return WRITE_STDIN_OK;
}

static void check_child_status(libvchan_t *vchan)
{
    int status = 0;
    if (local_pid >= 0) {
        /* this is not a socket-based service */
        pid_t pid = waitpid(local_pid, &status, WNOHANG);
        if (pid < 0) {
            perror("waitpid");
            do_exit(1);
        }
        if (pid == 0 || !WIFEXITED(status))
            return;
        status = WEXITSTATUS(status);
    }
    if (is_service)
        send_exit_code(vchan, status);
    do_exit(status);
}

static void select_loop(libvchan_t *vchan, int data_protocol_version, struct buffer *stdin_buf)
{
    fd_set select_set;
    fd_set wr_set;
    int max_fd;
    int ret;
    int vchan_fd;
    sigset_t selectmask;
    struct timespec zero_timeout = { 0, 0 };
    struct timespec select_timeout = { 10, 0 };

    sigemptyset(&selectmask);
    sigaddset(&selectmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &selectmask, NULL);
    sigemptyset(&selectmask);
    /* remember to set back to blocking mode before closing the FD - this may
     * be not the only copy and some processes may misbehave when get
     * nonblocking FD for input/output
     */
    set_nonblock(local_stdin_fd);

    for (;;) {
        vchan_fd = libvchan_fd_for_select(vchan);
        FD_ZERO(&select_set);
        FD_ZERO(&wr_set);
        FD_SET(vchan_fd, &select_set);
        max_fd = vchan_fd;
        if (local_stdout_fd != -1 &&
                (size_t)libvchan_buffer_space(vchan) > sizeof(struct msg_header)) {
            FD_SET(local_stdout_fd, &select_set);
            if (local_stdout_fd > max_fd)
                max_fd = local_stdout_fd;
        }
        if (local_stdout_fd == -1 &&
            (child_exited || (local_stdin_fd == -1 && local_pid == -1)))
            check_child_status(vchan);
        if (local_stdin_fd != -1 && buffer_len(stdin_buf)) {
            FD_SET(local_stdin_fd, &wr_set);
            if (local_stdin_fd > max_fd)
                max_fd = local_stdin_fd;
        }
        if ((local_stdin_fd == -1 || buffer_len(stdin_buf) == 0) &&
                libvchan_data_ready(vchan) > 0) {
            /* check for other FDs, but exit immediately */
            ret = pselect(max_fd + 1, &select_set, &wr_set, NULL,
                    &zero_timeout, &selectmask);
        } else
            ret = pselect(max_fd + 1, &select_set, &wr_set, NULL,
                    &select_timeout, &selectmask);
        if (ret < 0) {
            if (errno == EINTR && local_pid > 0) {
                continue;
            } else {
                perror("select");
                do_exit(1);
            }
        }
        if (ret == 0) {
            if (!libvchan_is_open(vchan)) {
                /* remote disconnected witout a proper signaling */
                do_exit(1);
            }
        }
        if (FD_ISSET(vchan_fd, &select_set))
            libvchan_wait(vchan);
        if (buffer_len(stdin_buf) &&
                local_stdin_fd != -1 &&
                FD_ISSET(local_stdin_fd, &wr_set)) {
            if (flush_client_data(local_stdin_fd, stdin_buf) == WRITE_STDIN_ERROR) {
                perror("write stdin");
                close(local_stdin_fd);
                local_stdin_fd = -1;
            }
        }
        while (libvchan_data_ready(vchan))
            if (handle_vchan_data(vchan, stdin_buf, data_protocol_version)
                    != WRITE_STDIN_OK)
                break;

        if (local_stdout_fd != -1
                && FD_ISSET(local_stdout_fd, &select_set))
            handle_input(vchan, data_protocol_version);
    }
}

_Noreturn static void usage(const char *const name)
{
    fprintf(stderr,
            "usage: %s [-w timeout] [-W] [-t] [-T] -d domain_name ["
            "-l local_prog|"
            "-c request_id,src_domain_name,src_domain_id|"
            "-e] remote_cmdline\n"
            "-e means exit after sending cmd,\n"
            "-t enables replacing problematic bytes with '_' in command output, -T is the same for stderr\n"
            "-W waits for connection end even in case of VM-VM (-c)\n"
            "-c: connect to existing process (response to trigger service call)\n"
            "-w timeout: override default connection timeout of 5s (set 0 for no timeout)\n",
            name);
    exit(1);
}

static void parse_connect(char *str, char **request_id,
        char **src_domain_name, int *src_domain_id)
{
    int i=0;
    char *token = NULL;
    char *separators = ",";

    token = strtok(str, separators);
    while (token)
    {
        switch (i)
        {
            case 0:
                *request_id = token;
                if (strlen(*request_id) >= sizeof(struct service_params)) {
                    fprintf(stderr, "Invalid -c parameter (request_id too long, max %lu)\n",
                            sizeof(struct service_params)-1);
                    exit(1);
                }
                break;
            case 1:
                *src_domain_name = token;
                break;
            case 2:
                *src_domain_id = atoi(token);
                break;
            default:
                goto bad_c_param;
        }
        token = strtok(NULL, separators);
        i++;
    }
    if (i == 3)
        return;
bad_c_param:
    fprintf(stderr, "Invalid -c parameter (should be: \"-c request_id,src_domain_name,src_domain_id\")\n");
    exit(1);
}

static void sigalrm_handler(int x __attribute__((__unused__)))
{
    fprintf(stderr, "vchan connection timeout\n");
    do_exit(1);
}

static void wait_for_vchan_client_with_timeout(libvchan_t *conn, int timeout) {
    struct timeval start_tv, now_tv, timeout_tv;

    if (timeout && gettimeofday(&start_tv, NULL) == -1) {
        perror("gettimeofday");
        do_exit(1);
    }
    while (conn && libvchan_is_open(conn) == VCHAN_WAITING) {
        if (timeout) {
            fd_set rdset;
            int fd = libvchan_fd_for_select(conn);

            /* calculate how much time left until connection timeout expire */
            if (gettimeofday(&now_tv, NULL) == -1) {
                perror("gettimeofday");
                do_exit(1);
            }
            timersub(&start_tv, &now_tv, &timeout_tv);
            timeout_tv.tv_sec += timeout;
            if (timeout_tv.tv_sec < 0) {
                fprintf(stderr, "vchan connection timeout\n");
                libvchan_close(conn);
                do_exit(1);
            }
            FD_ZERO(&rdset);
            FD_SET(fd, &rdset);
            switch (select(fd+1, &rdset, NULL, NULL, &timeout_tv)) {
                case -1:
                    if (errno == EINTR) {
                        break;
                    }
                    fprintf(stderr, "vchan connection error\n");
                    libvchan_close(conn);
                    do_exit(1);
                    break;
                case 0:
                    fprintf(stderr, "vchan connection timeout\n");
                    libvchan_close(conn);
                    do_exit(1);
            }
        }
        libvchan_wait(conn);
    }
}

static size_t compute_service_length(const char *const remote_cmdline, const char *const prog_name) {
    const size_t service_length = strlen(remote_cmdline) + 1;
    if (service_length < 2 || service_length > MAX_QREXEC_CMD_LEN) {
        /* This is arbitrary, but it helps reduce the risk of overflows in other code */
        fprintf(stderr, "Bad command: command line too long or empty: length %zu\n", service_length);
        usage(prog_name);
    }
    return service_length;
}

int main(int argc, char **argv)
{
    int opt;
    char *domname = NULL;
    libvchan_t *data_vchan = NULL;
    int data_port;
    int data_domain;
    int msg_type;
    int s;
    int just_exec = 0;
    int wait_connection_end = 0;
    int connect_existing = 0;
    char *local_cmdline = NULL;
    char *remote_cmdline = NULL;
    char *request_id;
    char *src_domain_name = NULL;
    int src_domain_id = 0; /* if not -c given, the process is run in dom0 */
    int connection_timeout = 5;
    struct service_params svc_params;
    int data_protocol_version;

    while ((opt = getopt(argc, argv, "d:l:ec:tTw:W")) != -1) {
        switch (opt) {
            case 'd':
                domname = xstrdup(optarg);
                break;
            case 'l':
                local_cmdline = xstrdup(optarg);
                break;
            case 'e':
                just_exec = 1;
                break;
            case 'c':
                parse_connect(optarg, &request_id, &src_domain_name, &src_domain_id);
                connect_existing = 1;
                is_service = 1;
                break;
            case 't':
                replace_chars_stdout = 1;
                break;
            case 'T':
                replace_chars_stderr = 1;
                break;
            case 'w':
                connection_timeout = atoi(optarg);
                break;
            case 'W':
                wait_connection_end = 1;
                break;
            default:
                usage(argv[0]);
        }
    }
    if (optind >= argc || !domname)
        usage(argv[0]);
    remote_cmdline = argv[optind];

    register_exec_func(&do_exec);

    if (just_exec + connect_existing + (local_cmdline != 0) > 1) {
        fprintf(stderr, "ERROR: only one of -e, -l, -c can be specified\n");
        usage(argv[0]);
    }

    if (strcmp(domname, "dom0") == 0 || strcmp(domname, "@adminvm") == 0) {
        if (connect_existing) {
            msg_type = MSG_SERVICE_CONNECT;
            strncpy(svc_params.ident, request_id, sizeof(svc_params.ident) - 1);
            svc_params.ident[sizeof(svc_params.ident) - 1] = '\0';
        } else {
            fprintf(stderr, "ERROR: when target domain is 'dom0', -c must be specified\n");
            usage(argv[0]);
        }
        if (src_domain_name == NULL) {
            fputs("internal error: src_domain_name should not be NULL here\n", stderr);
            abort();
        }
        set_remote_domain(src_domain_name);
        s = connect_unix_socket(src_domain_name);
        negotiate_connection_params(s,
                0, /* dom0 */
                msg_type,
                connect_existing ? (void*)&svc_params : (void*)remote_cmdline,
                connect_existing ? sizeof(svc_params) : compute_service_length(remote_cmdline, argv[0]),
                &data_domain,
                &data_port);

        struct buffer stdin_buffer;
        buffer_init(&stdin_buffer);
        prepare_local_fds(remote_cmdline, &stdin_buffer);
        if (connect_existing) {
            void (*old_handler)(int);

            /* libvchan_client_init is blocking and does not support connection
             * timeout, so use alarm(2) for that... */
            old_handler = signal(SIGALRM, sigalrm_handler);
            alarm(connection_timeout);
            data_vchan = libvchan_client_init(data_domain, data_port);
            alarm(0);
            signal(SIGALRM, old_handler);
        } else {
            data_vchan = libvchan_server_init(data_domain, data_port,
                    VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
            wait_for_vchan_client_with_timeout(data_vchan, connection_timeout);
        }
        if (!data_vchan || !libvchan_is_open(data_vchan)) {
            fprintf(stderr, "Failed to open data vchan connection\n");
            do_exit(1);
        }
        data_protocol_version = handle_agent_handshake(data_vchan, connect_existing);
        if (data_protocol_version < 0)
            do_exit(1);
        select_loop(data_vchan, data_protocol_version, &stdin_buffer);
    } else {
        msg_type = just_exec ? MSG_JUST_EXEC : MSG_EXEC_CMDLINE;
        s = connect_unix_socket(domname);
        negotiate_connection_params(s,
                src_domain_id,
                msg_type,
                remote_cmdline,
                compute_service_length(remote_cmdline, argv[0]),
                &data_domain,
                &data_port);
        if (wait_connection_end && connect_existing)
            /* save socket fd, 's' will be reused for the other qrexec-daemon
             * connection */
            wait_connection_end = s;
        else
            close(s);
        set_remote_domain(domname);
        struct buffer stdin_buffer;
        buffer_init(&stdin_buffer);
        prepare_local_fds(local_cmdline, &stdin_buffer);
        if (connect_existing) {
            s = connect_unix_socket(src_domain_name);
            send_service_connect(s, request_id, data_domain, data_port);
            close(s);
            if (wait_connection_end) {
                /* wait for EOF */
                fd_set read_fd;
                FD_ZERO(&read_fd);
                FD_SET(wait_connection_end, &read_fd);
                select(wait_connection_end+1, &read_fd, NULL, NULL, 0);
            }
        } else {
            data_vchan = libvchan_server_init(data_domain, data_port,
                    VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
            if (!data_vchan) {
                fprintf(stderr, "Failed to start data vchan server\n");
                do_exit(1);
            }
            wait_for_vchan_client_with_timeout(data_vchan, connection_timeout);
            if (!libvchan_is_open(data_vchan)) {
                fprintf(stderr, "Failed to open data vchan connection\n");
                do_exit(1);
            }
            data_protocol_version = handle_agent_handshake(data_vchan, 0);
            if (data_protocol_version < 0)
                do_exit(1);
            select_loop(data_vchan, data_protocol_version, &stdin_buffer);
        }
    }
    return 0;
}

// vim:ts=4:sw=4:et:
