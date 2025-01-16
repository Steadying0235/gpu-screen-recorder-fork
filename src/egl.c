#include "../include/egl.h"
#include "../include/window/window.h"
#include "../include/library_loader.h"
#include "../include/utils.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>
#include <unistd.h>
#include <sys/capability.h>

// TODO: rename gsr_egl to something else since this includes both egl and glx and in the future maybe vulkan too

#define GLX_DRAWABLE_TYPE                  0x8010
#define GLX_RENDER_TYPE                    0x8011
#define GLX_RGBA_BIT                       0x00000001
#define GLX_WINDOW_BIT                     0x00000001
#define GLX_PIXMAP_BIT                     0x00000002
#define GLX_BIND_TO_TEXTURE_RGBA_EXT       0x20D1
#define GLX_BIND_TO_TEXTURE_TARGETS_EXT    0x20D3
#define GLX_TEXTURE_2D_BIT_EXT             0x00000002
#define GLX_DOUBLEBUFFER                   5
#define GLX_RED_SIZE                       8
#define GLX_GREEN_SIZE                     9
#define GLX_BLUE_SIZE                      10
#define GLX_ALPHA_SIZE                     11
#define GLX_DEPTH_SIZE                     12
#define GLX_RGBA_TYPE                      0x8014

#define GLX_CONTEXT_PRIORITY_LEVEL_EXT    0x3100
#define GLX_CONTEXT_PRIORITY_HIGH_EXT     0x3101
#define GLX_CONTEXT_PRIORITY_MEDIUM_EXT   0x3102
#define GLX_CONTEXT_PRIORITY_LOW_EXT      0x3103

static void reset_cap_nice(void) {
    cap_t caps = cap_get_proc();
    if(!caps)
        return;

    const cap_value_t cap_to_remove = CAP_SYS_NICE;
    cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap_to_remove, CAP_CLEAR);
    cap_set_flag(caps, CAP_PERMITTED, 1, &cap_to_remove, CAP_CLEAR);
    cap_set_proc(caps);
    cap_free(caps);
}

// TODO: Create egl context without surface (in other words, x11/wayland agnostic, doesn't require x11/wayland dependency)
static bool gsr_egl_create_window(gsr_egl *self) {
    EGLConfig  ecfg;
    int32_t    num_config = 0;

    // TODO: Use EGL_OPENGL_ES_BIT as amd requires that for external texture, but that breaks software encoding
    const int32_t attr[] = {
        EGL_BUFFER_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE, EGL_NONE
    };

    const int32_t ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        //EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG, /* requires cap_sys_nice, ignored otherwise */
        EGL_NONE, EGL_NONE
    };

    // TODO: Use EGL_OPENGL_ES_API as amd requires that for external texture, but that breaks software encoding
    self->eglBindAPI(EGL_OPENGL_API);

    self->egl_display = self->eglGetDisplay((EGLNativeDisplayType)gsr_window_get_display(self->window));
    if(!self->egl_display) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglGetDisplay failed\n");
        goto fail;
    }

    if(!self->eglInitialize(self->egl_display, NULL, NULL)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglInitialize failed\n");
        goto fail;
    }

    if(!self->eglChooseConfig(self->egl_display, attr, &ecfg, 1, &num_config) || num_config != 1) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to find a matching config\n");
        goto fail;
    }

    self->egl_context = self->eglCreateContext(self->egl_display, ecfg, NULL, ctxattr);
    if(!self->egl_context) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create egl context\n");
        goto fail;
    }

    self->egl_surface = self->eglCreateWindowSurface(self->egl_display, ecfg, (EGLNativeWindowType)gsr_window_get_window(self->window), NULL);
    if(!self->egl_surface) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create window surface\n");
        goto fail;
    }

    if(!self->eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface, self->egl_context)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to make egl context current\n");
        goto fail;
    }

    reset_cap_nice();
    return true;

    fail:
    reset_cap_nice();
    gsr_egl_unload(self);
    return false;
}

static GLXFBConfig glx_fb_config_choose(gsr_egl *self, Display *display) {
    const int glx_visual_attribs[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        // TODO:
        //GLX_BIND_TO_TEXTURE_RGBA_EXT, 1,
        //GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 0,
        GLX_DEPTH_SIZE, 0,
        None, None
    };

    // TODO: Cleanup
    int c = 0;
    GLXFBConfig *fb_configs = self->glXChooseFBConfig(display, DefaultScreen(display), glx_visual_attribs, &c);
    if(c == 0 || !fb_configs)
        return NULL;

    return fb_configs[0];
}

static bool gsr_egl_switch_to_glx_context(gsr_egl *self) {
    // TODO: Cleanup
    assert(gsr_window_get_display_server(self->window) == GSR_DISPLAY_SERVER_X11);
    Display *display = gsr_window_get_display(self->window);
    const Window window = (Window)gsr_window_get_window(self->window);

    if(self->egl_context) {
        self->eglMakeCurrent(self->egl_display, NULL, NULL, NULL);
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

    self->glx_fb_config = glx_fb_config_choose(self, display);
    if(!self->glx_fb_config) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to find a suitable fb config\n");
        goto fail;
    }

    // TODO:
    //self->glx_context = self->glXCreateContextAttribsARB(display, self->glx_fb_config, NULL, True, context_attrib_list);
    self->glx_context = self->glXCreateNewContext(display, self->glx_fb_config, GLX_RGBA_TYPE, NULL, True);
    if(!self->glx_context) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create glx context\n");
        goto fail;
    }

    if(!self->glXMakeContextCurrent(display, window, window, self->glx_context)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to make glx context current\n");
        goto fail;
    }

    return true;

    fail:
    if(self->glx_context) {
        self->glXMakeContextCurrent(display, None, None, NULL);
        self->glXDestroyContext(display, self->glx_context);
        self->glx_context = NULL;
        self->glx_fb_config = NULL;
    }
    return false;
}

static bool gsr_egl_load_egl(gsr_egl *self, void *library) {
    const dlsym_assign required_dlsym[] = {
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
    self->eglQueryDisplayAttribEXT = (FUNC_eglQueryDisplayAttribEXT)self->eglGetProcAddress("eglQueryDisplayAttribEXT");
    self->eglQueryDeviceStringEXT = (FUNC_eglQueryDeviceStringEXT)self->eglGetProcAddress("eglQueryDeviceStringEXT");
    self->eglQueryDmaBufModifiersEXT = (FUNC_eglQueryDmaBufModifiersEXT)self->eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    if(!self->eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find eglExportDMABUFImageQueryMESA\n");
        return false;
    }

    if(!self->eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find eglExportDMABUFImageMESA\n");
        return false;
    }

    if(!self->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find glEGLImageTargetTexture2DOES\n");
        return false;
    }

    return true;
}

static bool gsr_egl_load_glx(gsr_egl *self, void *library) {
    const dlsym_assign required_dlsym[] = {
        { (void**)&self->glXGetProcAddress, "glXGetProcAddress" },
        { (void**)&self->glXChooseFBConfig, "glXChooseFBConfig" },
        { (void**)&self->glXMakeContextCurrent, "glXMakeContextCurrent" },
        { (void**)&self->glXCreateNewContext, "glXCreateNewContext" },
        { (void**)&self->glXDestroyContext, "glXDestroyContext" },
        { (void**)&self->glXSwapBuffers, "glXSwapBuffers" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGLX.so.0\n");
        return false;
    }

    self->glXCreateContextAttribsARB = (FUNC_glXCreateContextAttribsARB)self->glXGetProcAddress((const unsigned char*)"glXCreateContextAttribsARB");
    if(!self->glXCreateContextAttribsARB) {
        fprintf(stderr, "gsr error: gsr_egl_load_glx failed: could not find glXCreateContextAttribsARB\n");
        return false;
    }

    self->glXSwapIntervalEXT = (FUNC_glXSwapIntervalEXT)self->glXGetProcAddress((const unsigned char*)"glXSwapIntervalEXT");
    self->glXSwapIntervalMESA = (FUNC_glXSwapIntervalMESA)self->glXGetProcAddress((const unsigned char*)"glXSwapIntervalMESA");
    self->glXSwapIntervalSGI = (FUNC_glXSwapIntervalSGI)self->glXGetProcAddress((const unsigned char*)"glXSwapIntervalSGI");

    return true;
}

static bool gsr_egl_load_gl(gsr_egl *self, void *library) {
    const dlsym_assign required_dlsym[] = {
        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glFlush, "glFlush" },
        { (void**)&self->glFinish, "glFinish" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glClearColor, "glClearColor" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glTexParameteriv, "glTexParameteriv" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glGetTexImage, "glGetTexImage" },
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
        { (void**)&self->glDisable, "glDisable" },
        { (void**)&self->glBlendFunc, "glBlendFunc" },
        { (void**)&self->glGetUniformLocation, "glGetUniformLocation" },
        { (void**)&self->glUniform1f, "glUniform1f" },
        { (void**)&self->glUniform2f, "glUniform2f" },
        { (void**)&self->glDebugMessageCallback, "glDebugMessageCallback" },
        { (void**)&self->glScissor, "glScissor" },
        { (void**)&self->glReadPixels, "glReadPixels" },
        { (void**)&self->glMapBuffer, "glMapBuffer" },
        { (void**)&self->glUnmapBuffer, "glUnmapBuffer" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGL.so.1\n");
        return false;
    }

    return true;
}

#define GL_DEBUG_TYPE_ERROR               0x824C
#define GL_DEBUG_SEVERITY_NOTIFICATION    0x826B
static void debug_callback(unsigned int source, unsigned int type, unsigned int id, unsigned int severity, int length, const char* message, const void* userParam) {
    (void)source;
    (void)id;
    (void)length;
    (void)userParam;
    if(severity != GL_DEBUG_SEVERITY_NOTIFICATION)
        fprintf(stderr, "gsr info: gl callback: %s type = 0x%x, severity = 0x%x, message = %s\n", type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : "", type, severity, message);
}

bool gsr_egl_load(gsr_egl *self, gsr_window *window, bool is_monitor_capture, bool enable_debug) {
    memset(self, 0, sizeof(gsr_egl));
    self->context_type = GSR_GL_CONTEXT_TYPE_EGL;
    self->window = window;

    dlerror(); /* clear */
    self->egl_library = dlopen("libEGL.so.1", RTLD_LAZY);
    if(!self->egl_library) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libEGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    self->glx_library = dlopen("libGLX.so.0", RTLD_LAZY);

    self->gl_library = dlopen("libGL.so.1", RTLD_LAZY);
    if(!self->gl_library) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    if(!gsr_egl_load_egl(self, self->egl_library))
        goto fail;

    /* In some distros (alpine for example libGLX doesn't exist, but libGL can be used instead) */
    if(!gsr_egl_load_glx(self, self->glx_library ? self->glx_library : self->gl_library))
        goto fail;

    if(!gsr_egl_load_gl(self, self->gl_library))
        goto fail;

    if(!gsr_egl_proc_load_egl(self))
        goto fail;

    if(!gsr_egl_create_window(self))
        goto fail;

    if(!gl_get_gpu_info(self, &self->gpu_info))
        goto fail;

    if(self->eglQueryDisplayAttribEXT && self->eglQueryDeviceStringEXT) {
        intptr_t device = 0;
        if(self->eglQueryDisplayAttribEXT(self->egl_display, EGL_DEVICE_EXT, &device) && device)
            self->dri_card_path = self->eglQueryDeviceStringEXT((void*)device, EGL_DRM_DEVICE_FILE_EXT);
    }

    /* Nvfbc requires glx */
    if(gsr_window_get_display_server(self->window) == GSR_DISPLAY_SERVER_X11 && is_monitor_capture && self->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA) {
        self->context_type = GSR_GL_CONTEXT_TYPE_GLX;
        self->dri_card_path = NULL;
        if(!gsr_egl_switch_to_glx_context(self))
            goto fail;
    }

    self->glEnable(GL_BLEND);
    self->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if(enable_debug) {
        self->glEnable(GL_DEBUG_OUTPUT);
        self->glDebugMessageCallback(debug_callback, NULL);
    }

    return true;

    fail:
    gsr_egl_unload(self);
    return false;
}

void gsr_egl_unload(gsr_egl *self) {
    if(self->egl_context) {
        self->eglMakeCurrent(self->egl_display, NULL, NULL, NULL);
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

    if(self->glx_context) {
        assert(gsr_window_get_display_server(self->window) == GSR_DISPLAY_SERVER_X11);
        Display *display = gsr_window_get_display(self->window);
        self->glXMakeContextCurrent(display, None, None, NULL);
        self->glXDestroyContext(display, self->glx_context);
        self->glx_context = NULL;
        self->glx_fb_config = NULL;
    }

    if(self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = NULL;
    }

    if(self->glx_library) {
        dlclose(self->glx_library);
        self->glx_library = NULL;
    }

    if(self->gl_library) {
        dlclose(self->gl_library);
        self->gl_library = NULL;
    }

    memset(self, 0, sizeof(gsr_egl));
}

void gsr_egl_swap_buffers(gsr_egl *self) {
    /* This uses less cpu than swap buffer on nvidia */
    // TODO: Do these and remove swap
    //self->glFlush();
    //self->glFinish();
    if(self->egl_display) {
        self->eglSwapBuffers(self->egl_display, self->egl_surface);
    } else if(gsr_window_get_display_server(self->window) == GSR_DISPLAY_SERVER_X11) {
        Display *display = gsr_window_get_display(self->window);
        const Window window = (Window)gsr_window_get_window(self->window);
        self->glXSwapBuffers(display, window);
    }
}
