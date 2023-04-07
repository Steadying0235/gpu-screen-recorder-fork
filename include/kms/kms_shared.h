#ifndef GSR_KMS_SHARED_H
#define GSR_KMS_SHARED_H

#include <stdint.h>

typedef enum {
    KMS_REQUEST_TYPE_GET_KMS
} gsr_kms_request_type;

typedef enum {
    KMS_RESULT_OK,
    KMS_RESULT_INVALID_REQUEST,
    KMS_RESULT_FAILED_TO_OPEN_CARD,
    KMS_RESULT_INSUFFICIENT_PERMISSIONS,
    KMS_RESULT_FAILED_TO_GET_KMS,
    KMS_RESULT_NO_KMS_AVAILABLE,
    KMS_RESULT_FAILED_TO_SEND
} gsr_kms_result;

typedef struct {
    int type; /* gsr_kms_request_type */
    union {
        char card_path[255];
    } data;
} gsr_kms_request;

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t offset;
    uint32_t pixel_format;
    uint64_t modifier;
} gsr_kms_response_fd;

typedef struct {
    int result; /* gsr_kms_result */
    union {
        char err_msg[128];
        gsr_kms_response_fd fd;
    } data;
} gsr_kms_response;

#endif /* #define GSR_KMS_SHARED_H */
