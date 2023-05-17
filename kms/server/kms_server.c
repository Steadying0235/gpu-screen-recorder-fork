#include "../kms_shared.h"

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
#include <libdrm/drm_mode.h>

#define MAX_CONNECTORS 32

typedef struct {
    int drmfd;
    uint32_t plane_ids[GSR_KMS_MAX_PLANES];
    uint32_t connector_ids[GSR_KMS_MAX_PLANES];
    size_t num_plane_ids;
} gsr_drm;

typedef struct {
    uint32_t connector_id;
    uint64_t crtc_id;
} connector_crtc_pair;

typedef struct {
    connector_crtc_pair maps[MAX_CONNECTORS];
    int num_maps;
} connector_to_crtc_map;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int send_msg_to_client(int client_fd, gsr_kms_response *response) {
    struct iovec iov;
    iov.iov_base = response;
    iov.iov_len = sizeof(*response);

    struct msghdr response_message = {0};
    response_message.msg_iov = &iov;
    response_message.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int) * max_int(1, response->num_fds))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    if(response->num_fds > 0) {
        response_message.msg_control = cmsgbuf;
        response_message.msg_controllen = sizeof(cmsgbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&response_message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * response->num_fds);

        int *fds = (int*)CMSG_DATA(cmsg);
        for(int i = 0; i < response->num_fds; ++i) {
            fds[i] = response->fds[i].fd;
        }

        response_message.msg_controllen = cmsg->cmsg_len;
    }

    return sendmsg(client_fd, &response_message, 0);
}

static bool connector_get_property_by_name(int drmfd, drmModeConnectorPtr props, const char *name, uint64_t *result) {
    for(int i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmfd, props->props[i]);
        if(prop) {
            if(strcmp(name, prop->name) == 0) {
                *result = props->prop_values[i];
                drmModeFreeProperty(prop);
                return true;
            }
            drmModeFreeProperty(prop);
        }
    }
    return false;
}

static bool plane_is_cursor_plane(int drmfd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drmfd, plane_id, DRM_MODE_OBJECT_PLANE);
    if(!props)
        return false;

    for(uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmfd, props->props[i]);
        if(prop) {
            if(strcmp(prop->name, "type") == 0) {
                const uint64_t current_enum_value = props->prop_values[i];
                bool is_cursor = false;

                for(int j = 0; j < prop->count_enums; ++j) {
                    if(prop->enums[j].value == current_enum_value && strcmp(prop->enums[j].name, "Cursor") == 0) {
                        is_cursor = true;
                        break;
                    }

                }

                drmModeFreeProperty(prop);
                return is_cursor;
            }
            drmModeFreeProperty(prop);
        }
    }

    drmModeFreeObjectProperties(props);
    return false;
}

/* Returns 0 if not found */
static uint32_t get_connector_by_crtc_id(const connector_to_crtc_map *c2crtc_map, uint32_t crtc_id) {
    for(int i = 0; i < c2crtc_map->num_maps; ++i) {
        if(c2crtc_map->maps[i].crtc_id == crtc_id)
            return c2crtc_map->maps[i].connector_id;
    }
    return 0;
}

static int kms_get_plane_ids(gsr_drm *drm) {
    drmModePlaneResPtr planes = NULL;
    drmModeResPtr resources = NULL;
    int result = -1;

    connector_to_crtc_map c2crtc_map;
    c2crtc_map.num_maps = 0;

    if(drmSetClientCap(drm->drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        fprintf(stderr, "kms server error: drmSetClientCap DRM_CLIENT_CAP_UNIVERSAL_PLANES failed, error: %s\n", strerror(errno));
        goto error;
    }

    if(drmSetClientCap(drm->drmfd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "kms server warning: drmSetClientCap DRM_CLIENT_CAP_ATOMIC failed, error: %s. The wrong monitor may be captured as a result\n", strerror(errno));
    }

    planes = drmModeGetPlaneResources(drm->drmfd);
    if(!planes) {
        fprintf(stderr, "kms server error: failed to access planes, error: %s\n", strerror(errno));
        goto error;
    }

    resources = drmModeGetResources(drm->drmfd);
    if(resources) {
        for(int i = 0; i < resources->count_connectors && c2crtc_map.num_maps < MAX_CONNECTORS; ++i) {
            drmModeConnectorPtr connector = drmModeGetConnectorCurrent(drm->drmfd, resources->connectors[i]);
            if(connector) {
                uint64_t crtc_id = 0;
                connector_get_property_by_name(drm->drmfd, connector, "CRTC_ID", &crtc_id);

                c2crtc_map.maps[c2crtc_map.num_maps].connector_id = connector->connector_id;
                c2crtc_map.maps[c2crtc_map.num_maps].crtc_id = crtc_id;
                ++c2crtc_map.num_maps;

                drmModeFreeConnector(connector);
            }
        }
        drmModeFreeResources(resources);
    }

    for(uint32_t i = 0; i < planes->count_planes && drm->num_plane_ids < GSR_KMS_MAX_PLANES; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(drm->drmfd, planes->planes[i]);
        if(!plane) {
            fprintf(stderr, "kms server warning: failed to get drmModePlanePtr for plane %#x: %s (%d)\n", planes->planes[i], strerror(errno), errno);
            continue;
        }

        if(!plane->fb_id) {
            drmModeFreePlane(plane);
            continue;
        }

        if(plane_is_cursor_plane(drm->drmfd, plane->plane_id))
            continue;

        // TODO: Fallback to getfb(1)?
        drmModeFB2Ptr drmfb = drmModeGetFB2(drm->drmfd, plane->fb_id);
        if(drmfb) {
            drm->plane_ids[drm->num_plane_ids] = plane->plane_id;
            drm->connector_ids[drm->num_plane_ids] = get_connector_by_crtc_id(&c2crtc_map, plane->crtc_id);
            ++drm->num_plane_ids;
            drmModeFreeFB2(drmfb);
        }
        drmModeFreePlane(plane);
    }

    result = 0;

    error:
    if(planes)
        drmModeFreePlaneResources(planes);

    return result;
}

static bool drmfb_has_multiple_handles(drmModeFB2 *drmfb) {
    int num_handles = 0;
    for(uint32_t handle_index = 0; handle_index < 4 && drmfb->handles[handle_index]; ++handle_index) {
        ++num_handles;
    }
    return num_handles > 1;
}

static int kms_get_fb(gsr_drm *drm, gsr_kms_response *response) {
    int result = -1;

    response->result = KMS_RESULT_OK;
    response->err_msg[0] = '\0';
    response->num_fds = 0;

    for(size_t i = 0; i < drm->num_plane_ids && response->num_fds < GSR_KMS_MAX_PLANES; ++i) {
        drmModePlanePtr plane = NULL;
        drmModeFB2 *drmfb = NULL;

        plane = drmModeGetPlane(drm->drmfd, drm->plane_ids[i]);
        if(!plane) {
            response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
            snprintf(response->err_msg, sizeof(response->err_msg), "failed to get drm plane with id %u, error: %s\n", drm->plane_ids[i], strerror(errno));
            fprintf(stderr, "kms server error: %s\n", response->err_msg);
            goto next;
        }

        drmfb = drmModeGetFB2(drm->drmfd, plane->fb_id);
        if(!drmfb) {
            // Commented out for now because we get here if the cursor is moved to another monitor and we dont care about the cursor
            //response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
            //snprintf(response->err_msg, sizeof(response->err_msg), "drmModeGetFB2 failed, error: %s", strerror(errno));
            //fprintf(stderr, "kms server error: %s\n", response->err_msg);
            goto next;
        }

        if(!drmfb->handles[0]) {
            response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
            snprintf(response->err_msg, sizeof(response->err_msg), "drmfb handle is NULL");
            fprintf(stderr, "kms server error: %s\n", response->err_msg);
            goto next;
        }

        // TODO: Check if dimensions have changed by comparing width and height to previous time this was called.
        // TODO: Support other plane formats than rgb (with multiple planes, such as direct YUV420 on wayland).

        int fb_fd = -1;
        const int ret = drmPrimeHandleToFD(drm->drmfd, drmfb->handles[0], O_RDONLY, &fb_fd);
        if(ret != 0 || fb_fd == -1) {
            response->result = KMS_RESULT_FAILED_TO_GET_PLANE;
            snprintf(response->err_msg, sizeof(response->err_msg), "failed to get fd from drm handle, error: %s", strerror(errno));
            fprintf(stderr, "kms server error: %s\n", response->err_msg);
            continue;
        }

        response->fds[response->num_fds].fd = fb_fd;
        response->fds[response->num_fds].width = drmfb->width;
        response->fds[response->num_fds].height = drmfb->height;
        response->fds[response->num_fds].pitch = drmfb->pitches[0];
        response->fds[response->num_fds].offset = drmfb->offsets[0];
        response->fds[response->num_fds].pixel_format = drmfb->pixel_format;
        response->fds[response->num_fds].modifier = drmfb->modifier;
        response->fds[response->num_fds].connector_id = drm->connector_ids[i];
        response->fds[response->num_fds].is_combined_plane = drmfb_has_multiple_handles(drmfb);
        ++response->num_fds;

        next:
        if(drmfb)
            drmModeFreeFB2(drmfb);
        if(plane)
            drmModeFreePlane(plane);
    }

    if(response->num_fds > 0 || response->result == KMS_RESULT_OK) {
        result = 0;
    } else {
        for(int i = 0; i < response->num_fds; ++i) {
            close(response->fds[i].fd);
        }
        response->num_fds = 0;
    }

    return result;
}

static double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static void strncpy_safe(char *dst, const char *src, int len) {
    int src_len = strlen(src);
    int min_len = src_len;
    if(len - 1 < min_len)
        min_len = len - 1;
    memcpy(dst, src, min_len);
    dst[min_len] = '\0';
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
    drm.num_plane_ids = 0;
    drm.drmfd = open(card_path, O_RDONLY);
    if(drm.drmfd < 0) {
        fprintf(stderr, "kms server error: failed to open %s, error: %s", card_path, strerror(errno));
        return 2;
    }

    if(kms_get_plane_ids(&drm) != 0) {
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
        strncpy_safe(remote_addr.sun_path, domain_socket_path, sizeof(remote_addr.sun_path));
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
                    if(send_msg_to_client(socket_fd, &response) == -1)
                        fprintf(stderr, "kms server error: failed to respond to client KMS_REQUEST_TYPE_GET_KMS request\n");

                    for(int i = 0; i < response.num_fds; ++i) {
                        close(response.fds[i].fd);
                    }
                } else {
                    if(send_msg_to_client(socket_fd, &response) == -1)
                        fprintf(stderr, "kms server error: failed to respond to client KMS_REQUEST_TYPE_GET_KMS request\n");
                }

                break;
            }
            default: {
                gsr_kms_response response;
                response.result = KMS_RESULT_INVALID_REQUEST;
                snprintf(response.err_msg, sizeof(response.err_msg), "invalid request type %d, expected %d (%s)", request.type, KMS_REQUEST_TYPE_GET_KMS, "KMS_REQUEST_TYPE_GET_KMS");
                fprintf(stderr, "kms server error: %s\n", response.err_msg);
                if(send_msg_to_client(socket_fd, &response) == -1) {
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
