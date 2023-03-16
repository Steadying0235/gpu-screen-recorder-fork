#ifndef GSR_VAAPI_H
#define GSR_VAAPI_H

#include <stdint.h>
#include <stdbool.h>

typedef void* VADisplay;
typedef int VAStatus;
typedef unsigned int VAGenericID;
typedef VAGenericID VASurfaceID;

typedef struct {
    /** Pixel format fourcc of the whole surface (VA_FOURCC_*). */
    uint32_t fourcc;
    /** Width of the surface in pixels. */
    uint32_t width;
    /** Height of the surface in pixels. */
    uint32_t height;
    /** Number of distinct DRM objects making up the surface. */
    uint32_t num_objects;
    /** Description of each object. */
    struct {
        /** DRM PRIME file descriptor for this object. */
        int fd;
        /** Total size of this object (may include regions which are
         *  not part of the surface). */
        uint32_t size;
        /** Format modifier applied to this object. */
        uint64_t drm_format_modifier;
    } objects[4];
    /** Number of layers making up the surface. */
    uint32_t num_layers;
    /** Description of each layer in the surface. */
    struct {
        /** DRM format fourcc of this layer (DRM_FOURCC_*). */
        uint32_t drm_format;
        /** Number of planes in this layer. */
        uint32_t num_planes;
        /** Index in the objects array of the object containing each
         *  plane. */
        uint32_t object_index[4];
        /** Offset within the object of each plane. */
        uint32_t offset[4];
        /** Pitch of each plane. */
        uint32_t pitch[4];
    } layers[4];
} VADRMPRIMESurfaceDescriptor;

#define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2      0x40000000
#define VA_EXPORT_SURFACE_READ_WRITE                0x0003
#define VA_EXPORT_SURFACE_SEPARATE_LAYERS           0x0004

typedef struct {
    void *library;
    
    VAStatus (*vaExportSurfaceHandle)(VADisplay dpy, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void *descriptor);
    VAStatus (*vaSyncSurface)(VADisplay dpy, VASurfaceID render_target);
} gsr_vaapi;

bool gsr_vaapi_load(gsr_vaapi *self);
void gsr_vaapi_unload(gsr_vaapi *self);

#endif /* GSR_VAAPI_H */
