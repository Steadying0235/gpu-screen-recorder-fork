#ifndef GSR_COLOR_CONVERSION_H
#define GSR_COLOR_CONVERSION_H

#include "shader.h"
#include "vec2.h"

typedef enum {
    GSR_SOURCE_COLOR_RGB
} gsr_source_color;

typedef enum {
    GSR_DESTINATION_COLOR_NV12
} gsr_destination_color;

typedef struct {
    gsr_egl *egl;

    gsr_source_color source_color;
    gsr_destination_color destination_color;

    unsigned int source_textures[2];
    int num_source_textures;

    unsigned int destination_textures[2];
    int num_destination_textures;

    float rotation;

    vec2f position;
    vec2f size;
} gsr_color_conversion_params;

typedef struct {
    gsr_egl *egl;
    gsr_shader shaders[2];

    unsigned int source_textures[2];
    unsigned int destination_textures[2];

    unsigned int framebuffers[2];

    unsigned int vertex_array_object_id;
    unsigned int vertex_buffer_object_id;
} gsr_color_conversion;

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params);
void gsr_color_conversion_deinit(gsr_color_conversion *self);

int gsr_color_conversion_update(gsr_color_conversion *self, int width, int height);

#endif /* GSR_COLOR_CONVERSION_H */
