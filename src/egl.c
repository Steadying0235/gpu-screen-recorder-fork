#include "../include/egl.h"
#include "../include/library_loader.h"
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <assert.h>

#include <wayland-client.h>
#include <wayland-egl.h>
//#include "../external/wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include <unistd.h>

#if 0
static struct wl_compositor *compositor = NULL;
static struct wl_output *output = NULL;
static struct zwlr_export_dmabuf_manager_v1 *export_manager = NULL;
static struct zwlr_export_dmabuf_frame_v1 *current_frame = NULL;
//static struct wl_shell *shell = NULL;

struct window {
	EGLContext egl_context;
	struct wl_surface *surface;
	//struct wl_shell_surface *shell_surface;
	struct wl_egl_window *egl_window;
	EGLSurface egl_surface;
};

static void output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	fprintf(stderr, "output geometry, make: %s, model: %s\n", make, model);
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	
}

static void output_handle_done(void* data, struct wl_output *wl_output) {
	/* Nothing to do */
}

static void output_handle_scale(void* data, struct wl_output *wl_output,
		int32_t factor) {
	/* Nothing to do */
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
};
#endif

static void registry_add_object (void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    struct wl_compositor **wayland_compositor = data;
	if (strcmp(interface, "wl_compositor") == 0) {
        if(*wayland_compositor) {
            wl_compositor_destroy(*wayland_compositor);
            *wayland_compositor = NULL;
        }
		*wayland_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    }/* else if(strcmp(interface, wl_output_interface.name) == 0) {
        fprintf(stderr, "wayland output, name: %u\n", name);
        output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        wl_output_add_listener(output, &output_listener, NULL);
	} else if(strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
        export_manager = wl_registry_bind(registry, name, &zwlr_export_dmabuf_manager_v1_interface, 1);
    }*/
    //fprintf(stderr, "interface: %s\n", interface);
}

static void registry_remove_object (void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener registry_listener = {&registry_add_object, &registry_remove_object};

#if 0
static void register_cb(gsr_egl *egl);

static void frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t width, uint32_t height, uint32_t offset_x, uint32_t offset_y,
		uint32_t buffer_flags, uint32_t flags, uint32_t format,
		uint32_t mod_high, uint32_t mod_low, uint32_t num_objects) {
    gsr_egl *egl = data;
	//fprintf(stderr, "frame start, width: %u, height: %u, offset x: %u, offset y: %u, format: %u, num objects: %u\n", width, height, offset_x, offset_y, format, num_objects);
    egl->width = width;
    egl->height = height;
    egl->pixel_format = format;
    egl->modifier = ((uint64_t)mod_high << 32) | mod_low;
    current_frame = frame;
}

static void frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t index, int32_t fd, uint32_t size, uint32_t offset,
		uint32_t stride, uint32_t plane_index) {
    // TODO: What if we get multiple objects? then we get multiple fd per frame
    gsr_egl *egl = data;
    //egl->fd = fd;
    egl->pitch = stride;
    egl->offset = offset;
	//fprintf(stderr, "new frame, fd: %d, index: %u, size: %u, offset: %u, stride: %u, plane_index: %u\n", fd, index, size, offset, stride, plane_index);
    close(fd);
}


static void frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	register_cb(data);
}

static void frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t reason) {
    register_cb(data);
}


static const struct zwlr_export_dmabuf_frame_v1_listener frame_listener = {
	.frame = frame_start,
	.object = frame_object,
	.ready = frame_ready,
	.cancel = frame_cancel,
};

static struct zwlr_export_dmabuf_frame_v1 *frame_callback = NULL;
static void register_cb(gsr_egl *egl) {
    bool with_cursor = false;
    frame_callback = zwlr_export_dmabuf_manager_v1_capture_output(export_manager, with_cursor, output);
    zwlr_export_dmabuf_frame_v1_add_listener(frame_callback, &frame_listener, egl);
}
#endif

// TODO: Create egl context without surface (in other words, x11/wayland agnostic, doesn't require x11/wayland dependency)
static bool gsr_egl_create_window(gsr_egl *self, bool wayland) {
    EGLConfig  ecfg;
    int32_t    num_config = 0;

    EGLDisplay egl_display = NULL;
    EGLSurface egl_surface = NULL;
    EGLContext egl_context = NULL;

    Window x11_window = None;

    struct wl_registry *wayland_registry = NULL;
    struct wl_compositor *wayland_compositor = NULL;
    struct wl_surface *wayland_surface = NULL;
    void *wayland_dpy = NULL;
    void *wayland_window = NULL;

    const int32_t attr[] = {
        EGL_BUFFER_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    const int32_t ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    if(wayland) {
        wayland_dpy = wl_display_connect(NULL);
        if(!wayland_dpy) {
            fprintf(stderr, "gsr error: gsr_egl_create_window failed: wl_display_connect failed\n");
            goto fail;
        }

        wayland_registry = wl_display_get_registry(wayland_dpy); // TODO: Error checking
        wl_registry_add_listener(wayland_registry, &registry_listener, &wayland_compositor); // TODO: Error checking

        // Fetch globals
        wl_display_roundtrip(wayland_dpy);

        // fetch wl_output
        wl_display_roundtrip(wayland_dpy);

        if(!wayland_compositor) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to find compositor\n");
            goto fail;
        }

        /*if(!output) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to find output\n");
            goto fail;
        }

        if(!export_manager) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to find export manager\n");
            goto fail;
        }*/
    } else {
        x11_window = XCreateWindow(self->x11_dpy, DefaultRootWindow(self->x11_dpy), 0, 0, 16, 16, 0, CopyFromParent, InputOutput, CopyFromParent, 0, NULL);

        if(!x11_window) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl window\n");
            goto fail;
        }
    }

    self->eglBindAPI(EGL_OPENGL_ES_API);

    egl_display = self->eglGetDisplay(wayland_dpy ? (EGLNativeDisplayType)wayland_dpy : (EGLNativeDisplayType)self->x11_dpy);
    if(!egl_display) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglGetDisplay failed\n");
        goto fail;
    }

    if(!self->eglInitialize(egl_display, NULL, NULL)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglInitialize failed\n");
        goto fail;
    }
    
    if(!self->eglChooseConfig(egl_display, attr, &ecfg, 1, &num_config) || num_config != 1) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to find a matching config\n");
        goto fail;
    }
    
    egl_context = self->eglCreateContext(egl_display, ecfg, NULL, ctxattr);
    if(!egl_context) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create egl context\n");
        goto fail;
    }

    if(wayland) {
        wayland_surface = wl_compositor_create_surface(wayland_compositor);
        wayland_window = wl_egl_window_create(wayland_surface, 16, 16);
        egl_surface = self->eglCreateWindowSurface(egl_display, ecfg, (EGLNativeWindowType)wayland_window, NULL);
    } else {
        egl_surface = self->eglCreateWindowSurface(egl_display, ecfg, (EGLNativeWindowType)x11_window, NULL);
    }

    if(!egl_surface) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create window surface\n");
        goto fail;
    }

    if(!self->eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to make context current\n");
        goto fail;
    }

    self->egl_display = egl_display;
    self->egl_surface = egl_surface;
    self->egl_context = egl_context;

    self->x11_window = x11_window;

    self->wayland_dpy = wayland_dpy;
    self->wayland_window = wayland_window;
    self->wayland_surface = wayland_surface;
    self->wayland_compositor = wayland_compositor;
    self->wayland_registry = wayland_registry;
    return true;

    fail:
    if(egl_context)
        self->eglDestroyContext(egl_display, egl_context);
    if(egl_surface)
        self->eglDestroySurface(egl_display, egl_surface);
    if(egl_display)
        self->eglTerminate(egl_display);
    if(x11_window)
        XDestroyWindow(self->x11_dpy, x11_window);
    if(wayland_window)
        wl_egl_window_destroy(wayland_window);
    if(wayland_surface)
        wl_surface_destroy(wayland_surface);
    if(wayland_compositor)
        wl_compositor_destroy(wayland_compositor);
    if(wayland_registry)
        wl_registry_destroy(wayland_registry);
    if(wayland_dpy)
        wl_display_disconnect(wayland_dpy);
    return false;
}

static bool gsr_egl_load_egl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->eglGetError, "eglGetError" },
        { (void**)&self->eglGetDisplay, "eglGetDisplay" },
        { (void**)&self->eglInitialize, "eglInitialize" },
        { (void**)&self->eglTerminate, "eglTerminate" },
        { (void**)&self->eglChooseConfig, "eglChooseConfig" },
        { (void**)&self->eglCreateWindowSurface, "eglCreateWindowSurface" },
        { (void**)&self->eglCreateContext, "eglCreateContext" },
        { (void**)&self->eglMakeCurrent, "eglMakeCurrent" },
        { (void**)&self->eglCreateImage, "eglCreateImage" },
        { (void**)&self->eglDestroyContext, "eglDestroyContext" },
        { (void**)&self->eglDestroySurface, "eglDestroySurface" },
        { (void**)&self->eglDestroyImage, "eglDestroyImage" },
        { (void**)&self->eglSwapInterval, "eglSwapInterval" },
        { (void**)&self->eglSwapBuffers, "eglSwapBuffers" },
        { (void**)&self->eglBindAPI, "eglBindAPI" },
        { (void**)&self->eglGetProcAddress, "eglGetProcAddress" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libEGL.so.1\n");
        return false;
    }

    return true;
}

static bool gsr_egl_proc_load_egl(gsr_egl *self) {
    self->eglExportDMABUFImageQueryMESA = (FUNC_eglExportDMABUFImageQueryMESA)self->eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    self->eglExportDMABUFImageMESA = (FUNC_eglExportDMABUFImageMESA)self->eglGetProcAddress("eglExportDMABUFImageMESA");
    self->glEGLImageTargetTexture2DOES = (FUNC_glEGLImageTargetTexture2DOES)self->eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if(!self->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find glEGLImageTargetTexture2DOES\n");
        return false;
    }

    return true;
}

static bool gsr_egl_load_gl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glClearColor, "glClearColor" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glCopyImageSubData, "glCopyImageSubData" },
        { (void**)&self->glClearTexImage, "glClearTexImage" },
        { (void**)&self->glGenFramebuffers, "glGenFramebuffers" },
        { (void**)&self->glBindFramebuffer, "glBindFramebuffer" },
        { (void**)&self->glDeleteFramebuffers, "glDeleteFramebuffers" },
        { (void**)&self->glViewport, "glViewport" },
        { (void**)&self->glFramebufferTexture2D, "glFramebufferTexture2D" },
        { (void**)&self->glDrawBuffers, "glDrawBuffers" },
        { (void**)&self->glCheckFramebufferStatus, "glCheckFramebufferStatus" },
        { (void**)&self->glBindBuffer, "glBindBuffer" },
        { (void**)&self->glGenBuffers, "glGenBuffers" },
        { (void**)&self->glBufferData, "glBufferData" },
        { (void**)&self->glBufferSubData, "glBufferSubData" },
        { (void**)&self->glDeleteBuffers, "glDeleteBuffers" },
        { (void**)&self->glGenVertexArrays, "glGenVertexArrays" },
        { (void**)&self->glBindVertexArray, "glBindVertexArray" },
        { (void**)&self->glDeleteVertexArrays, "glDeleteVertexArrays" },
        { (void**)&self->glCreateProgram, "glCreateProgram" },
        { (void**)&self->glCreateShader, "glCreateShader" },
        { (void**)&self->glAttachShader, "glAttachShader" },
        { (void**)&self->glBindAttribLocation, "glBindAttribLocation" },
        { (void**)&self->glCompileShader, "glCompileShader" },
        { (void**)&self->glLinkProgram, "glLinkProgram" },
        { (void**)&self->glShaderSource, "glShaderSource" },
        { (void**)&self->glUseProgram, "glUseProgram" },
        { (void**)&self->glGetProgramInfoLog, "glGetProgramInfoLog" },
        { (void**)&self->glGetShaderiv, "glGetShaderiv" },
        { (void**)&self->glGetShaderInfoLog, "glGetShaderInfoLog" },
        { (void**)&self->glDeleteProgram, "glDeleteProgram" },
        { (void**)&self->glDeleteShader, "glDeleteShader" },
        { (void**)&self->glGetProgramiv, "glGetProgramiv" },
        { (void**)&self->glVertexAttribPointer, "glVertexAttribPointer" },
        { (void**)&self->glEnableVertexAttribArray, "glEnableVertexAttribArray" },
        { (void**)&self->glDrawArrays, "glDrawArrays" },
        { (void**)&self->glEnable, "glEnable" },
        { (void**)&self->glBlendFunc, "glBlendFunc" },
        { (void**)&self->glGetUniformLocation, "glGetUniformLocation" },
        { (void**)&self->glUniform1f, "glUniform1f" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGL.so.1\n");
        return false;
    }

    return true;
}

bool gsr_egl_load(gsr_egl *self, Display *dpy, bool wayland) {
    memset(self, 0, sizeof(gsr_egl));
    self->x11_dpy = dpy;

    void *egl_lib = NULL;
    void *gl_lib = NULL;

    dlerror(); /* clear */
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libEGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    gl_lib = dlopen("libGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    if(!gsr_egl_load_egl(self, egl_lib))
        goto fail;

    if(!gsr_egl_load_gl(self, gl_lib))
        goto fail;

    if(!gsr_egl_proc_load_egl(self))
        goto fail;

    if(!gsr_egl_create_window(self, wayland))
        goto fail;

    self->glEnable(GL_BLEND);
    self->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    self->egl_library = egl_lib;
    self->gl_library = gl_lib;
    return true;

    fail:
    if(egl_lib)
        dlclose(egl_lib);
    if(gl_lib)
        dlclose(gl_lib);
    memset(self, 0, sizeof(gsr_egl));
    return false;
}

void gsr_egl_unload(gsr_egl *self) {
    if(self->egl_context) {
        self->eglDestroyContext(self->egl_display, self->egl_context);
        self->egl_context = NULL;
    }

    if(self->egl_surface) {
        self->eglDestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = NULL;
    }

    if(self->egl_display) {
        self->eglTerminate(self->egl_display);
        self->egl_display = NULL;
    }

    if(self->x11_window) {
        XDestroyWindow(self->x11_dpy, self->x11_window);
        self->x11_window = None;
    }

    if(self->wayland_window) {
        wl_egl_window_destroy(self->wayland_window);
        self->wayland_window = NULL;
    }

    if(self->wayland_surface) {
        wl_surface_destroy(self->wayland_surface);
        self->wayland_surface = NULL;
    }

    if(self->wayland_compositor) {
        wl_compositor_destroy(self->wayland_compositor);
        self->wayland_compositor = NULL;
    }

    if(self->wayland_registry) {
        wl_registry_destroy(self->wayland_registry);
        self->wayland_registry = NULL;
    }

    if(self->wayland_dpy) {
        wl_display_disconnect(self->wayland_dpy);
        self->wayland_dpy = NULL;
    }

    if(self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = NULL;
    }

    if(self->gl_library) {
        dlclose(self->gl_library);
        self->gl_library = NULL;
    }

    memset(self, 0, sizeof(gsr_egl));
}

void gsr_egl_update(gsr_egl *self) {
    if(!self->wayland_dpy)
        return;

    wl_display_dispatch(self->wayland_dpy);
}

void gsr_egl_cleanup_frame(gsr_egl *self) {
    if(!self->wayland_dpy)
        return;

    if(self->fd > 0) {
        close(self->fd);
        self->fd = 0;
    }

    /*if(current_frame) {
        zwlr_export_dmabuf_frame_v1_destroy(current_frame);
        current_frame = NULL;
    }*/
}
