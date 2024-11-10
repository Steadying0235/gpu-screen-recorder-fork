#ifndef GSR_PIPEWIRE_AUDIO_H
#define GSR_PIPEWIRE_AUDIO_H

#include <pipewire/thread-loop.h>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <spa/utils/hook.h>

#include <stdbool.h>

#define GSR_PIPEWIRE_AUDIO_MAX_STREAM_NODES 64
#define GSR_PIPEWIRE_AUDIO_MAX_PORTS 64
#define GSR_PIPEWIRE_AUDIO_MAX_REQUESTED_LINKS 32

typedef enum {
    GSR_PIPEWIRE_AUDIO_NODE_TYPE_STREAM_OUTPUT, /* Application audio */
    GSR_PIPEWIRE_AUDIO_NODE_TYPE_STREAM_INPUT,  /* Audio recording input */
    GSR_PIPEWIRE_AUDIO_NODE_TYPE_SINK
} gsr_pipewire_audio_node_type;

typedef struct {
    uint32_t id;
    char *name;
    gsr_pipewire_audio_node_type type;
} gsr_pipewire_audio_node;

typedef enum {
    GSR_PIPEWIRE_AUDIO_PORT_DIRECTION_INPUT,
    GSR_PIPEWIRE_AUDIO_PORT_DIRECTION_OUTPUT
} gsr_pipewire_audio_port_direction;

typedef struct {
    uint32_t id;
    uint32_t node_id;
    gsr_pipewire_audio_port_direction direction;
    char *name;
} gsr_pipewire_audio_port;

typedef enum {
    GSR_PIPEWIRE_AUDIO_LINK_OUTPUT_TYPE_STREAM,
    GSR_PIPEWIRE_AUDIO_LINK_OUTPUT_TYPE_SINK
} gsr_pipewire_audio_link_output_type;

typedef struct {
    char **app_names;
    int num_app_names;
    char *output_name;
    bool inverted;
    gsr_pipewire_audio_link_output_type output_type;
} gsr_pipewire_audio_requested_link;

typedef struct {
    struct pw_thread_loop *thread_loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    int server_version_sync;

    gsr_pipewire_audio_node stream_nodes[GSR_PIPEWIRE_AUDIO_MAX_STREAM_NODES];
    int num_stream_nodes;

    gsr_pipewire_audio_port ports[GSR_PIPEWIRE_AUDIO_MAX_PORTS];
    int num_ports;

    gsr_pipewire_audio_requested_link requested_links[GSR_PIPEWIRE_AUDIO_MAX_REQUESTED_LINKS];
    int num_requested_links;
} gsr_pipewire_audio;

bool gsr_pipewire_audio_init(gsr_pipewire_audio *self);
void gsr_pipewire_audio_deinit(gsr_pipewire_audio *self);

/*
    This function links audio source outputs from applications that match the name |app_names_output| to the input
    that matches the name |stream_name_input|.
    If an application or a new application starts outputting audio after this function is called and the app name matches
    then it will automatically link the audio sources.
    |app_names_output| and |stream_name_input| are case-insensitive matches.
*/
bool gsr_pipewire_audio_add_link_from_apps_to_stream(gsr_pipewire_audio *self, const char **app_names_output, int num_app_names_output, const char *stream_name_input);
/*
    This function links audio source outputs from all applications except the ones that match the name |app_names_output| to the input
    that matches the name |stream_name_input|.
    If an application or a new application starts outputting audio after this function is called and the app name doesn't matche
    then it will automatically link the audio sources.
    |app_names_output| and |stream_name_input| are case-insensitive matches.
*/
bool gsr_pipewire_audio_add_link_from_apps_to_stream_inverted(gsr_pipewire_audio *self, const char **app_names_output, int num_app_names_output, const char *stream_name_input);

/*
    This function links audio source outputs from applications that match the name |app_names_output| to the input
    that matches the name |sink_name_input|.
    If an application or a new application starts outputting audio after this function is called and the app name matches
    then it will automatically link the audio sources.
    |app_names_output| and |sink_name_input| are case-insensitive matches.
*/
bool gsr_pipewire_audio_add_link_from_apps_to_sink(gsr_pipewire_audio *self, const char **app_names_output, int num_app_names_output, const char *sink_name_input);
/*
    This function links audio source outputs from all applications except the ones that match the name |app_names_output| to the input
    that matches the name |sink_name_input|.
    If an application or a new application starts outputting audio after this function is called and the app name doesn't matche
    then it will automatically link the audio sources.
    |app_names_output| and |sink_name_input| are case-insensitive matches.
*/
bool gsr_pipewire_audio_add_link_from_apps_to_sink_inverted(gsr_pipewire_audio *self, const char **app_names_output, int num_app_names_output, const char *sink_name_input);

/* Return true to continue */
typedef bool (*gsr_pipewire_audio_app_query_callback)(const char *app_name, void *userdata);
void gsr_pipewire_audio_for_each_app(gsr_pipewire_audio *self, gsr_pipewire_audio_app_query_callback callback, void *userdata);

#endif /* GSR_PIPEWIRE_AUDIO_H */
