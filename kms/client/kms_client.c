#include "kms_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/capability.h>

static bool is_inside_flatpak(void) {
    return getenv("FLATPAK_ID") != NULL;
}

static int send_msg_to_server(int server_fd, gsr_kms_request *request) {
    struct iovec iov;
    iov.iov_base = request;
    iov.iov_len = sizeof(*request);

    struct msghdr request_message = {0};
    request_message.msg_iov = &iov;
    request_message.msg_iovlen = 1;

    return sendmsg(server_fd, &request_message, 0);
}

static int recv_msg_from_server(int server_fd, gsr_kms_response *response) {
    struct iovec iov;
    iov.iov_base = response;
    iov.iov_len = sizeof(*response);

    struct msghdr response_message = {0};
    response_message.msg_iov = &iov;
    response_message.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    response_message.msg_control = cmsgbuf;
    response_message.msg_controllen = sizeof(cmsgbuf);

    int res = recvmsg(server_fd, &response_message, MSG_WAITALL);
    if(res <= 0)
        return res;

    if(response->result == KMS_RESULT_OK)
        response->data.fd.fd = 0;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&response_message);
    if(cmsg) {
        fprintf(stderr, "got cmsg, %d\n", cmsg->cmsg_type);
        if(cmsg->cmsg_type == SCM_RIGHTS) {
            int kms_fd = 0;
            memcpy(&kms_fd, CMSG_DATA(cmsg), sizeof(int));
            fprintf(stderr, "kms fd: %d\n", kms_fd);
            response->data.fd.fd = kms_fd;
        }
    }

    return res;
}

int gsr_kms_client_init(gsr_kms_client *self, const char *card_path) {
    self->kms_server_pid = -1;
    self->card_path = NULL;
    self->socket_fd = -1;
    self->client_fd = -1;
    self->socket_path[0] = '\0';
    struct sockaddr_un local_addr = {0};
    struct sockaddr_un remote_addr = {0};

    // This doesn't work on nixos, but we dont want to use $PATH because we want to make this as safe as possible by running pkexec
    // on a path that only root can modify. If we use "gsr-kms-server" instead then $PATH can be modified in ~/.bashrc for example
    // which will overwrite the path to gsr-kms-server and the user can end up running a malicious program that pretends to be gsr-kms-server.
    // If there is a safe way to do this on nixos, then please tell me; or use gpu-screen-recorder flatpak instead.
    const char *server_filepath = "/usr/bin/gsr-kms-server";
    bool has_perm = 0;
    bool inside_flatpak = is_inside_flatpak();
    if(!inside_flatpak) {
        if(access("/usr/bin/gsr-kms-server", F_OK) != 0) {
            fprintf(stderr, "gsr error: gsr_kms_client_init: /usr/bin/gsr-kms-server not found, please install gpu-screen-recorder first\n");
            return -1;
        }

        if(geteuid() == 0) {
            has_perm = true;
        } else {
            cap_t kms_server_cap = cap_get_file(server_filepath);
            if(kms_server_cap) {
                cap_flag_value_t res = 0;
                cap_get_flag(kms_server_cap, CAP_SYS_ADMIN, CAP_PERMITTED, &res);
                if(res == CAP_SET) {
                    //fprintf(stderr, "has permission!\n");
                    has_perm = true;
                } else {
                    //fprintf(stderr, "No permission:(\n");
                }
                cap_free(kms_server_cap);
            } else {
                if(errno == ENODATA)
                    fprintf(stderr, "gsr info: gsr_kms_client_init: gsr-kms-server is missing sys_admin cap and will require root authentication. To bypass this automatically, run: sudo setcap cap_sys_admin+ep '%s'\n", server_filepath);
                else
                    fprintf(stderr, "gsr info: gsr_kms_client_init: failed to get cap\n");
            }
        }
    }

    self->card_path = strdup(card_path);
    if(!self->card_path) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to duplicate card_path\n");
        goto err;
    }

    strcpy(self->socket_path, "/tmp/gsr-kms-socket-XXXXXX");
    if(!tmpnam(self->socket_path)) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: mkstemp failed, error: %s\n", strerror(errno));
        goto err;
    }

    self->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(self->socket_fd == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: socket failed, error: %s\n", strerror(errno));
        goto err;
    }

    local_addr.sun_family = AF_UNIX;
    strncpy(local_addr.sun_path, self->socket_path, sizeof(local_addr.sun_path));
    if(bind(self->socket_fd, (struct sockaddr*)&local_addr, sizeof(local_addr.sun_family) + strlen(local_addr.sun_path)) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to bind socket, error: %s\n", strerror(errno));
        goto err;
    }

    if(listen(self->socket_fd, 1) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to listen on socket, error: %s\n", strerror(errno));
        goto err;
    }

    pid_t pid = fork();
    if(pid == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: fork failed, error: %s\n", strerror(errno));
        goto err;
    } else if(pid == 0) { /* child */
        if(inside_flatpak) {
            const char *args[] = { "flatpak-spawn", "--host", "pkexec", "flatpak", "run", "--command=gsr-kms-server", "com.dec05eba.gpu_screen_recorder", self->socket_path, NULL };
            execvp(args[0], (char *const*)args);
        } else if(has_perm) {
            const char *args[] = { server_filepath, self->socket_path, NULL };
            execvp(args[0], (char *const*)args);
        } else {
            const char *args[] = { "pkexec", server_filepath, self->socket_path, NULL };
            execvp(args[0], (char *const*)args);
        }
        fprintf(stderr, "gsr error: gsr_kms_client_init: execvp failed, error: %s\n", strerror(errno));
        _exit(127);
    } else { /* parent */
        self->kms_server_pid = pid;
    }

    fprintf(stderr, "gsr info: gsr_kms_client_init: waiting for client to connect\n");
    for(;;) {
        struct timeval tv;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(self->socket_fd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; // 100 ms

        int select_res = select(1 + self->socket_fd, &rfds, NULL, NULL, &tv);
        if(select_res > 0) {
            socklen_t sock_len = 0;
            self->client_fd = accept(self->socket_fd, (struct sockaddr*)&remote_addr, &sock_len);
            if(self->client_fd == -1) {
                fprintf(stderr, "gsr error: gsr_kms_client_init: accept failed on socket, error: %s\n", strerror(errno));
                goto err;
            }
            break;
        } else {
            int status;
            int wait_result = waitpid(self->kms_server_pid, &status, WNOHANG);
            if(wait_result != 0) {
                fprintf(stderr, "gsr error: gsr_kms_client_init: kms server died or never started, error: %s\n", strerror(errno));
                goto err;
            }
        }
    }
    fprintf(stderr, "gsr info: gsr_kms_client_init: client connected\n");

    return 0;

    err:
    gsr_kms_client_deinit(self);
    return -1;
}

void gsr_kms_client_deinit(gsr_kms_client *self) {
    if(self->card_path) {
        free(self->card_path);
        self->card_path = NULL;
    }

    if(self->client_fd != -1) {
        close(self->client_fd);
        self->client_fd = -1;
    }

    if(self->socket_fd != -1) {
        close(self->socket_fd);
        self->socket_fd = -1;
    }

    if(self->kms_server_pid != -1) {
        kill(self->kms_server_pid, SIGINT);
        int status;
        waitpid(self->kms_server_pid, &status, 0);
        self->kms_server_pid = -1;
    }

    if(self->socket_path[0] != '\0') {
        remove(self->socket_path);
        self->socket_path[0] = '\0';
    }
}

int gsr_kms_client_get_kms(gsr_kms_client *self, gsr_kms_response *response) {
    response->result = KMS_RESULT_FAILED_TO_SEND;
    strcpy(response->data.err_msg, "failed to send");

    gsr_kms_request request;
    request.type = KMS_REQUEST_TYPE_GET_KMS;
    strcpy(request.data.card_path, self->card_path);
    if(send_msg_to_server(self->client_fd, &request) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_get_kms: failed to send request message to server\n");
        return -1;
    }

    const int recv_res = recv_msg_from_server(self->client_fd, response);
    if(recv_res == 0) {
        fprintf(stderr, "gsr warning: gsr_kms_client_get_kms: kms server shut down\n");
        return -1;
    } else if(recv_res == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_get_kms: failed to receive response\n");
        return -1;
    }

    return 0;
}
