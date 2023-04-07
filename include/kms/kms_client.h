#ifndef GSR_KMS_CLIENT_H
#define GSR_KMS_CLIENT_H

#include "kms_shared.h"
#include <sys/types.h>

typedef struct {
    pid_t kms_server_pid;
    int socket_fd;
    int client_fd;
    char socket_path[27];
    char *card_path;
} gsr_kms_client;

/* |card_path| should be a path to card, for example /dev/dri/card0 */
int gsr_kms_client_init(gsr_kms_client *self, const char *card_path, const char *program_dir);
void gsr_kms_client_deinit(gsr_kms_client *self);

int gsr_kms_client_get_kms(gsr_kms_client *self, gsr_kms_response *response);

#endif /* #define GSR_KMS_CLIENT_H */
