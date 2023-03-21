#ifndef GSR_VAAPI_H
#define GSR_VAAPI_H

#include <stdint.h>
#include <stdbool.h>

// To prevent hwcontext_vaapi.h from including va.h.. An ugly hack
#define _VA_H_

// These definitions are copied from va.h, which is licensed under MIT

typedef void* VADisplay;
typedef int VAStatus;

typedef unsigned int VAGenericID;
typedef VAGenericID VAConfigID;
typedef VAGenericID VAContextID;
typedef VAGenericID VASurfaceID;

#define VA_STATUS_SUCCESS           0x00000000

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

#define VA_INVALID_ID       0xffffffff
#define VA_INVALID_SURFACE  VA_INVALID_ID

/** \brief Generic value types. */
typedef enum  {
    VAGenericValueTypeInteger = 1,      /**< 32-bit signed integer. */
    VAGenericValueTypeFloat,            /**< 32-bit floating-point value. */
    VAGenericValueTypePointer,          /**< Generic pointer type */
    VAGenericValueTypeFunc              /**< Pointer to function */
} VAGenericValueType;

/** \brief Generic function type. */
typedef void (*VAGenericFunc)(void);

/** \brief Generic value. */
typedef struct _VAGenericValue {
    /** \brief Value type. See #VAGenericValueType. */
    VAGenericValueType  type;
    /** \brief Value holder. */
    union {
        /** \brief 32-bit signed integer. */
        int32_t             i;
        /** \brief 32-bit float. */
        float           f;
        /** \brief Generic pointer. */
        void           *p;
        /** \brief Pointer to function. */
        VAGenericFunc   fn;
    }                   value;
} VAGenericValue;

/** @name Surface attribute flags */
/**@{*/
/** \brief Surface attribute is not supported. */
#define VA_SURFACE_ATTRIB_NOT_SUPPORTED 0x00000000
/** \brief Surface attribute can be got through vaQuerySurfaceAttributes(). */
#define VA_SURFACE_ATTRIB_GETTABLE      0x00000001
/** \brief Surface attribute can be set through vaCreateSurfaces(). */
#define VA_SURFACE_ATTRIB_SETTABLE      0x00000002
/**@}*/

/** \brief Surface attribute types. */
typedef enum {
    VASurfaceAttribNone = 0,
    /**
     * \brief Pixel format as a FOURCC (int, read/write).
     *
     * When vaQuerySurfaceAttributes() is called, the driver will return one
     * PixelFormat attribute per supported pixel format.
     *
     * When provided as an input to vaCreateSurfaces(), the driver will
     * allocate a surface with the provided pixel format.
     */
    VASurfaceAttribPixelFormat,
    /** \brief Minimal width in pixels (int, read-only). */
    VASurfaceAttribMinWidth,
    /** \brief Maximal width in pixels (int, read-only). */
    VASurfaceAttribMaxWidth,
    /** \brief Minimal height in pixels (int, read-only). */
    VASurfaceAttribMinHeight,
    /** \brief Maximal height in pixels (int, read-only). */
    VASurfaceAttribMaxHeight,
    /** \brief Surface memory type expressed in bit fields (int, read/write). */
    VASurfaceAttribMemoryType,
    /** \brief External buffer descriptor (pointer, write).
     *
     * Refer to the documentation for the memory type being created to
     * determine what descriptor structure to pass here.  If not otherwise
     * stated, the common VASurfaceAttribExternalBuffers should be used.
     */
    VASurfaceAttribExternalBufferDescriptor,
    /** \brief Surface usage hint, gives the driver a hint of intended usage
     *  to optimize allocation (e.g. tiling) (int, read/write). */
    VASurfaceAttribUsageHint,
    /** \brief List of possible DRM format modifiers (pointer, write).
     *
     * The value must be a pointer to a VADRMFormatModifierList. This can only
     * be used when allocating a new buffer, it's invalid to use this attribute
     * when importing an existing buffer.
     */
    VASurfaceAttribDRMFormatModifiers,
    /** \brief Number of surface attributes. */
    VASurfaceAttribCount
} VASurfaceAttribType;

/** \brief Surface attribute. */
typedef struct _VASurfaceAttrib {
    /** \brief Type. */
    VASurfaceAttribType type;
    /** \brief Flags. See "Surface attribute flags". */
    uint32_t        flags;
    /** \brief Value. See "Surface attribute types" for the expected types. */
    VAGenericValue      value;
} VASurfaceAttrib;

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
