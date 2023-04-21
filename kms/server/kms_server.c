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
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2

typedef struct {
    int drmfd;
    uint32_t plane_id;
} gsr_drm;

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

static int kms_get_plane_id(gsr_drm *drm) {
    drmModePlaneResPtr planes = NULL;
    int result = -1;
    int64_t max_size = 0;
    uint32_t best_plane_match = UINT32_MAX;

    if(drmSetClientCap(drm->drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        fprintf(stderr, "kms server error: drmSetClientCap failed, error: %s\n", strerror(errno));
        goto error;
    }

    planes = drmModeGetPlaneResources(drm->drmfd);
    if(!planes) {
        fprintf(stderr, "kms server error: failed to access planes, error: %s\n", strerror(errno));
        goto error;
    }

    for(uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(drm->drmfd, planes->planes[i]);
        if(!plane) {
            fprintf(stderr, "kms server warning: failed to get drmModePlanePtr for plane %#x: %s (%d)\n", planes->planes[i], strerror(errno), errno);
            continue;
        }

        if(!plane->fb_id) {
            drmModeFreePlane(plane);
            continue;
        }

        // TODO: Fallback to getfb(1)?
        drmModeFB2Ptr drmfb = drmModeGetFB2(drm->drmfd, plane->fb_id);
        if(drmfb) {
            const int64_t plane_size = (int64_t)drmfb->width * (int64_t)drmfb->height;
            if(drmfb->handles[0] && plane_size >= max_size) {
                max_size = plane_size;
                best_plane_match = plane->plane_id;
            }
            drmModeFreeFB2(drmfb);
        }
        drmModeFreePlane(plane);
    }

    if(best_plane_match == UINT32_MAX || max_size == 0) {
        fprintf(stderr, "kms server error: failed to find a usable plane\n");
        goto error;
    }

    drm->plane_id = best_plane_match;
    result = 0;

    error:
    if(planes)
        drmModeFreePlaneResources(planes);

    return result;
}

static int kms_get_fb(gsr_drm *drm, gsr_kms_response *response) {
    drmModePlanePtr plane = NULL;
    drmModeFB2 *drmfb = NULL;
    int result = -1;

    response->result = KMS_RESULT_OK;
    response->data.fd.fd = 0;
    response->data.fd.width = 0;
    response->data.fd.height = 0;

    plane = drmModeGetPlane(drm->drmfd, drm->plane_id);
    if(!plane) {
        response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "failed to get drm plane with id %u, error: %s\n", drm->plane_id, strerror(errno));
        fprintf(stderr, "kms server error: %s\n", response->data.err_msg);
        goto error;
    }

    drmfb = drmModeGetFB2(drm->drmfd, plane->fb_id);
    if(!drmfb) {
        response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "drmModeGetFB2 failed, error: %s", strerror(errno));
        fprintf(stderr, "kms server error: %s\n", response->data.err_msg);
        goto error;
    }

    if(!drmfb->handles[0]) {
        response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "drmfb handle is NULL");
        fprintf(stderr, "kms server error: %s\n", response->data.err_msg);
        goto error;
    }

    // TODO: Check if dimensions have changed by comparing width and height to previous time this was called.
    // TODO: Support other plane formats than rgb (with multiple planes, such as direct YUV420 on wayland).

    int fb_fd = -1;
    const int ret = drmPrimeHandleToFD(drm->drmfd, drmfb->handles[0], O_RDONLY, &fb_fd);
    if(ret != 0 || fb_fd == -1) {
        response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
        snprintf(response->data.err_msg, sizeof(response->data.err_msg), "failed to get fd from drm handle, error: %s", strerror(errno));
        fprintf(stderr, "kms server error: %s\n", response->data.err_msg);
        goto error;
    }

    response->data.fd.fd = fb_fd;
    response->data.fd.width = drmfb->width;
    response->data.fd.height = drmfb->height;
    response->data.fd.pitch = drmfb->pitches[0];
    response->data.fd.offset = drmfb->offsets[0];
    response->data.fd.pixel_format = drmfb->pixel_format;
    response->data.fd.modifier = drmfb->modifier;
    result = 0;

    error:
    if(drmfb)
        drmModeFreeFB2(drmfb);
    if(plane)
        drmModeFreePlane(plane);

    return result;
}

static double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "usage: kms_server <domain_socket_path> <card_path>\n");
        return 1;
    }

    const char *domain_socket_path = argv[1];
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(socket_fd == -1) {
        fprintf(stderr, "kms server error: failed to create socket, error: %s\n", strerror(errno));
        return 2;
    }

    const char *card_path = argv[2];

    gsr_drm drm;
    drm.plane_id = 0;
    drm.drmfd = open(card_path, O_RDONLY);
    if(drm.drmfd < 0) {
        fprintf(stderr, "kms server error: failed to open %s, error: %s", card_path, strerror(errno));
        return 2;
    }

    if(kms_get_plane_id(&drm) != 0) {
        close(drm.drmfd);
        return 2;
    }

    fprintf(stderr, "kms server info: connecting to the client\n");
    bool connected = false;
    const double connect_timeout_sec = 5.0;
    const double start_time = clock_get_monotonic_seconds();
    while(clock_get_monotonic_seconds() - start_time < connect_timeout_sec) {
        struct sockaddr_un remote_addr = {0};
        remote_addr.sun_family = AF_UNIX;
        strncpy(remote_addr.sun_path, domain_socket_path, sizeof(remote_addr.sun_path));
        // TODO: Check if parent disconnected
        if(connect(socket_fd, (struct sockaddr*)&remote_addr, sizeof(remote_addr.sun_family) + strlen(remote_addr.sun_path)) == -1) {
            if(errno == ECONNREFUSED || errno == ENOENT) {
                goto next;
            } else if(errno == EISCONN) {
                connected = true;
                break;
            }

            fprintf(stderr, "kms server error: connect failed, error: %s (%d)\n", strerror(errno), errno);
            close(drm.drmfd);
            return 2;
        }

        next:
        usleep(30 * 1000); // 30 milliseconds
    }

    if(connected) {
        fprintf(stderr, "kms server info: connected to the client\n");
    } else {
        fprintf(stderr, "kms server error: failed to connect to the client in %f seconds\n", connect_timeout_sec);
        close(drm.drmfd);
        return 2;
    }

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

        switch(request.type) {
            case KMS_REQUEST_TYPE_GET_KMS: {
                gsr_kms_response response;
                
                if(kms_get_fb(&drm, &response) == 0) {
                    if(send_msg_to_client(socket_fd, &response, &response.data.fd.fd, 1) == -1)
                        fprintf(stderr, "kms server error: failed to respond to client KMS_REQUEST_TYPE_GET_KMS request\n");
                    close(response.data.fd.fd);
                } else {
                    if(send_msg_to_client(socket_fd, &response, NULL, 0) == -1)
                        fprintf(stderr, "kms server error: failed to respond to client KMS_REQUEST_TYPE_GET_KMS request\n");
                }

                break;
            }
            default: {
                gsr_kms_response response;
                response.result = KMS_RESULT_INVALID_REQUEST;
                snprintf(response.data.err_msg, sizeof(response.data.err_msg), "invalid request type %d, expected %d (%s)", request.type, KMS_REQUEST_TYPE_GET_KMS, "KMS_REQUEST_TYPE_GET_KMS");
                fprintf(stderr, "kms server error: %s\n", response.data.err_msg);
                if(send_msg_to_client(socket_fd, &response, NULL, 0) == -1) {
                    fprintf(stderr, "kms server error: failed to respond to client request\n");
                    break;
                }
                break;
            }
        }
    }

    done:
    close(drm.drmfd);
    close(socket_fd);
    return res;
}
