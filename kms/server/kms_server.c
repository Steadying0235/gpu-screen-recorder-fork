#include "../kms_shared.h"

#include <asm-generic/socket.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int send_msg_to_client(int client_fd, gsr_kms_response *response, int *fds, int num_fds) {
    struct iovec iov;
    iov.iov_base = response;
    iov.iov_len = sizeof(*response);

    struct msghdr response_message = {0};
    response_message.msg_iov = &iov;
    response_message.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int)) * max_int(1, num_fds)];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    if(num_fds > 0) {
        response_message.msg_control = cmsgbuf;
        response_message.msg_controllen = sizeof(cmsgbuf);

        int total_msg_len = 0;
        struct cmsghdr *cmsg = NULL;
        for(int i = 0; i < num_fds; ++i) {
            if(i == 0)
                cmsg = CMSG_FIRSTHDR(&response_message);
            else
                cmsg = CMSG_NXTHDR(&response_message, cmsg);

            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &fds[i], sizeof(int));
            total_msg_len += cmsg->cmsg_len;
        }

        response_message.msg_controllen = total_msg_len;
    }

    return sendmsg(client_fd, &response_message, 0);
}

static int get_kms(const char *card_path, gsr_kms_response *response) {
    response->result = KMS_RESULT_OK;
    response->data.fd.fd = 0;
    response->data.fd.width = 0;
    response->data.fd.height = 0;

    const int drmfd = open(card_path, O_RDONLY);
    if (drmfd < 0) {
        response->result = KMS_RESULT_FAILED_TO_OPEN_CARD;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "failed to open %s, error: %s", card_path, strerror(errno));
        return -1;
    }

    if (0 != drmSetClientCap(drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        response->result = KMS_RESULT_INSUFFICIENT_PERMISSIONS;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "drmSetClientCap failed, error: %s", strerror(errno));
        close(drmfd);
        return -1;
    }

    drmModePlaneResPtr planes = drmModeGetPlaneResources(drmfd);
    if (!planes) {
        response->result = KMS_RESULT_FAILED_TO_GET_KMS;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "failed to access planes, error: %s", strerror(errno));
        close(drmfd);
        return -1;
    }

    fprintf(stderr, "DRM planes %d:\n", planes->count_planes);
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(drmfd, planes->planes[i]);
        if (!plane) {
            fprintf(stderr, "Cannot get drmModePlanePtr for plane %#x: %s (%d)\n", planes->planes[i], strerror(errno), errno);
            continue;
        }

        fprintf(stderr, "\t%d: fb_id=%#x\n", i, plane->fb_id);

        if (!plane->fb_id)
            goto plane_continue;

        drmModeFB2Ptr drmfb = drmModeGetFB2(drmfd, plane->fb_id);
        if (!drmfb) {
            fprintf(stderr, "Cannot get drmModeFBPtr for fb %#x: %s (%d)\n", plane->fb_id, strerror(errno), errno);
        } else {
            if (!drmfb->handles[0]) {
                fprintf(stderr, "\t\tFB handle for fb %#x is NULL\n", plane->fb_id);
                fprintf(stderr, "\t\tPossible reason: not permitted to get FB handles. Do `sudo setcap cap_sys_admin+ep`\n");
            } else {
                int fb_fd = -1;
                const int ret = drmPrimeHandleToFD(drmfd, drmfb->handles[0], 0, &fb_fd);
                if (ret != 0 || fb_fd == -1) {
                    fprintf(stderr, "Cannot get fd for fb %#x handle %#x: %s (%d)\n", plane->fb_id, drmfb->handles[0], strerror(errno), errno);
                } else if(drmfb->width * drmfb->height > response->data.fd.width * response->data.fd.height) {
                    if(response->data.fd.fd != 0) {
                        close(response->data.fd.fd);
                        response->data.fd.fd = 0;
                    }

                    response->data.fd.fd = fb_fd;
                    response->data.fd.width = drmfb->width;
                    response->data.fd.height = drmfb->height;
                    response->data.fd.pitch = drmfb->pitches[0];
                    response->data.fd.offset = drmfb->offsets[0];
                    response->data.fd.pixel_format = drmfb->pixel_format;
                    response->data.fd.modifier = drmfb->modifier;
                    fprintf(stderr, "kms width: %u, height: %u, pixel format: %u, modifier: %lu\n", response->data.fd.width, response->data.fd.height, response->data.fd.pixel_format, response->data.fd.modifier);
                } else {
                    close(fb_fd);
                }
            }
            drmModeFreeFB2(drmfb);
        }

    plane_continue:
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    close(drmfd);

    if(response->data.fd.fd == 0) {
        response->result = KMS_RESULT_NO_KMS_AVAILABLE;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "no kms found");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "usage: kms_server <domain_socket_path>\n");
        return 1;
    }

    const char *domain_socket_path = argv[1];
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(socket_fd == -1) {
        fprintf(stderr, "kms server error: failed to create socket, error: %s\n", strerror(errno));
        return 2;
    }

    fprintf(stderr, "kms server info: connecting to the server\n");
    for(;;) {
        struct sockaddr_un remote_addr = {0};
        remote_addr.sun_family = AF_UNIX;
        strncpy(remote_addr.sun_path, domain_socket_path, sizeof(remote_addr.sun_path));
        // TODO: Check if parent disconnected
        if(connect(socket_fd, (struct sockaddr*)&remote_addr, sizeof(remote_addr.sun_family) + strlen(remote_addr.sun_path)) == -1) {
            if(errno == ECONNREFUSED || errno == ENOENT)
                continue; // Host not ready yet? TODO: sleep
            if(errno == EISCONN) // TODO?
                break;
            fprintf(stderr, "kms server error: connect failed, error: %s (%d)\n", strerror(errno), errno);
            return 2;
        }
    }
    fprintf(stderr, "kms server info: connected to the server\n");

    int res = 0;
    for(;;) {
        gsr_kms_request request;
        struct iovec iov;
        iov.iov_base = &request;
        iov.iov_len = sizeof(request);

        struct msghdr request_message = {0};
        request_message.msg_iov = &iov;
        request_message.msg_iovlen = 1;
        const int recv_res = recvmsg(socket_fd, &request_message, MSG_WAITALL);
        if(recv_res == 0) {
            fprintf(stderr, "kms server info: kms client shutdown, shutting down the server\n");
            res = 3;
            goto done;
        } else if(recv_res == -1) {
            const int err = errno;
            fprintf(stderr, "kms server error: failed to read all data in client request (error: %s), ignoring\n", strerror(err));
            if(err == EBADF) {
                fprintf(stderr, "kms server error: invalid client fd, shutting down the server\n");
                res = 3;
                goto done;
            }
            continue;
        }
        request.data.card_path[254] = '\0';

        switch(request.type) {
            case KMS_REQUEST_TYPE_GET_KMS: {
                gsr_kms_response response;
                int kms_fd = 0;
                if (get_kms(request.data.card_path, &response) == 0) {
                    kms_fd = response.data.fd.fd;
                }

                if(send_msg_to_client(socket_fd, &response, &kms_fd, kms_fd == 0 ? 0 : 1) == -1) {
                    fprintf(stderr, "kms server error: failed to respond to client KMS_REQUEST_TYPE_GET_KMS request\n");
                    if(kms_fd != 0)
                        close(kms_fd);
                    break;
                }

                if(kms_fd != 0)
                    close(kms_fd);

                break;
            }
            default: {
                gsr_kms_response response;
                response.result = KMS_RESULT_INVALID_REQUEST;
                snprintf(response.data.err_msg, sizeof(response.data.err_msg), "invalid request type %d, expected %d (%s)", request.type, KMS_REQUEST_TYPE_GET_KMS, "KMS_REQUEST_TYPE_GET_KMS");
                fprintf(stderr, "%s\n", response.data.err_msg);
                if(send_msg_to_client(socket_fd, &response, NULL, 0) == -1) {
                    fprintf(stderr, "kms server error: failed to respond to client request\n");
                    break;
                }
                break;
            }
        }
    }

    done:
    close(socket_fd);
    return res;
}
