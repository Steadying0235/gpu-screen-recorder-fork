#include "../include/color_conversion.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define MAX_SHADERS 2
#define MAX_FRAMEBUFFERS 2

#define ROTATE_Z   "mat4 rotate_z(in float angle) {\n"                        \
                   "    return mat4(cos(angle), -sin(angle), 0.0, 0.0,\n"     \
                   "                sin(angle),  cos(angle), 0.0, 0.0,\n"     \
                   "                0.0,           0.0,      1.0, 0.0,\n"     \
                   "                0.0,           0.0,      0.0, 1.0);\n"    \
                   "}\n"

#define RGB_TO_YUV "const mat4 RGBtoYUV = mat4(0.257,  0.439, -0.148, 0.0,\n" \
                   "                           0.504, -0.368, -0.291, 0.0,\n" \
                   "                           0.098, -0.071,  0.439, 0.0,\n" \
                   "                           0.0625, 0.500,  0.500, 1.0);"

static int load_shader_y(gsr_shader *shader, gsr_egl *egl, float rotation) {
    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                   \n"
        "in vec2 pos;                                      \n"
        "in vec2 texcoords;                                \n"
        "out vec2 texcoords_out;                           \n"
        ROTATE_Z
        "void main()                                       \n"
        "{                                                 \n"
        "  texcoords_out = texcoords;                      \n"
        "  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0) * rotate_z(%f);    \n"
        "}                                                 \n", rotation);

    char fragment_shader[] =
        "#version 300 es                                                                 \n"
        "precision mediump float;                                                        \n"
        "in vec2 texcoords_out;                                                          \n"
        "uniform sampler2D tex1;                                                         \n"
        "out vec4 FragColor;                                                             \n"
        RGB_TO_YUV
        "void main()                                                                     \n"
        "{                                                                               \n"
        "  FragColor.x = (RGBtoYUV * vec4(texture(tex1, texcoords_out).rgb, 1.0)).x;     \n"
        "}                                                                               \n";

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    return 0;
}

static unsigned int load_shader_uv(gsr_shader *shader, gsr_egl *egl, float rotation) {
    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
        ROTATE_Z
        "void main()                                     \n"
        "{                                               \n"
        "  texcoords_out = texcoords;                    \n"
        "  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0) * rotate_z(%f) * vec4(0.5, 0.5, 1.0, 1.0) - vec4(0.5, 0.5, 0.0, 0.0);   \n"
        "}                                               \n", rotation);

    char fragment_shader[] =
        "#version 300 es                                                                       \n"
        "precision mediump float;                                                              \n"
        "in vec2 texcoords_out;                                                                \n"
        "uniform sampler2D tex1;                                                               \n"
        "out vec4 FragColor;                                                                   \n"
        RGB_TO_YUV
        "void main()                                                                           \n"
        "{                                                                                     \n"
        "  FragColor.xy = (RGBtoYUV * vec4(texture(tex1, texcoords_out).rgb, 1.0)).zy;         \n"
        "}                                                                                     \n";

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    return 0;
}

static int loader_framebuffers(gsr_color_conversion *self) {
    const unsigned int draw_buffer = GL_COLOR_ATTACHMENT0;
    self->egl->glGenFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);

    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->destination_textures[0], 0);
    self->egl->glDrawBuffers(1, &draw_buffer);
    if(self->egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for Y\n");
        goto err;
    }

    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
    self->egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->destination_textures[1], 0);
    self->egl->glDrawBuffers(1, &draw_buffer);
    if(self->egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for UV\n");
        goto err;
    }

    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;

    err:
    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return -1;
}

static int create_vertices(gsr_color_conversion *self, vec2f position, vec2f size) {
    const float vertices[] = {
        -1.0f,  1.0f,  position.x,          position.y + size.y,
        -1.0f, -1.0f,  position.x,          position.y,
        1.0f, -1.0f,   position.x + size.x, position.y,

        -1.0f,  1.0f,  position.x,          position.y + size.y,
        1.0f, -1.0f,   position.x + size.x, position.y,
        1.0f,  1.0f,   position.x + size.x, position.y + size.y
    };

    self->egl->glGenVertexArrays(1, &self->vertex_array_object_id);
    self->egl->glGenBuffers(1, &self->vertex_buffer_object_id);
    self->egl->glBindVertexArray(self->vertex_array_object_id);
    self->egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->egl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), &vertices, GL_STATIC_DRAW);

    self->egl->glEnableVertexAttribArray(0);
    self->egl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    self->egl->glEnableVertexAttribArray(1);
    self->egl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    self->egl->glBindVertexArray(0);
    return 0;
}

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params) {
    assert(params);
    assert(params->egl);
    memset(self, 0, sizeof(*self));
    self->egl = params->egl;

    if(params->num_source_textures != 1) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 1 source texture for source color RGB, got %d source texture(s)\n", params->num_source_textures);
        return -1;
    }

    if(params->num_destination_textures != 2) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 2 destination textures for destination color NV12, got %d destination texture(s)\n", params->num_destination_textures);
        return -1;
    }

    if(load_shader_y(&self->shaders[0], self->egl, params->rotation) != 0) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to loader Y shader\n");
        goto err;
    }

    if(load_shader_uv(&self->shaders[1], self->egl, params->rotation) != 0) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to loader UV shader\n");
        goto err;
    }

    self->source_textures[0] = params->source_textures[0];
    self->destination_textures[0] = params->destination_textures[0];
    self->destination_textures[1] = params->destination_textures[1];

    if(loader_framebuffers(self) != 0)
        goto err;

    if(create_vertices(self, params->position, params->size) != 0)
        goto err;

    return 0;

    err:
    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gsr_color_conversion_deinit(self);
    return -1;
}

void gsr_color_conversion_deinit(gsr_color_conversion *self) {
    if(!self->egl)
        return;

    if(self->vertex_buffer_object_id) {
        self->egl->glDeleteBuffers(1, &self->vertex_buffer_object_id);
        self->vertex_buffer_object_id = 0;
    }

    if(self->vertex_array_object_id) {
        self->egl->glDeleteVertexArrays(1, &self->vertex_array_object_id);
        self->vertex_array_object_id = 0;
    }

    self->egl->glDeleteFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);
    for(int i = 0; i < MAX_FRAMEBUFFERS; ++i) {
        self->framebuffers[i] = 0;
    }

    for(int i = 0; i < MAX_SHADERS; ++i) {
        gsr_shader_deinit(&self->shaders[i]);
    }

    self->egl = NULL;
}

int gsr_color_conversion_update(gsr_color_conversion *self, int width, int height) {
    self->egl->glBindVertexArray(self->vertex_array_object_id);
    self->egl->glViewport(0, 0, width, height);
    self->egl->glBindTexture(GL_TEXTURE_2D, self->source_textures[0]);

    {
        self->egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

        gsr_shader_use(&self->shaders[0]);
        self->egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    {
        self->egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

        gsr_shader_use(&self->shaders[1]);
        self->egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    self->egl->glBindVertexArray(0);
    gsr_shader_use_none(&self->shaders[0]);
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);
    self->egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}
