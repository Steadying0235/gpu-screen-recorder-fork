#include "../../include/capture/xcomposite_drm.h"
#include "../../include/egl.h"
#include "../../include/vaapi.h"
#include "../../include/window_texture.h"
#include "../../include/time.h"
#include <stdlib.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
//#include <drm_fourcc.h>
#include <assert.h>
/* TODO: Proper error checks and cleanups */

typedef struct {
    gsr_capture_xcomposite_drm_params params;
    Display *dpy;
    XEvent xev;
    bool created_hw_frame;

    vec2i window_pos;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    gsr_egl egl;
    gsr_vaapi vaapi;

    int fourcc;
    int num_planes;
    uint64_t modifiers;
    int dmabuf_fd;
    int32_t stride;
    int32_t offset;

    unsigned int target_textures[2];

    unsigned int FramebufferNameY;
    unsigned int FramebufferNameUV; // TODO: Remove
    unsigned int quadVAO;

    unsigned int shader_y;
    unsigned int shader_uv;

    VADisplay va_dpy;
} gsr_capture_xcomposite_drm;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static bool drm_create_codec_context(gsr_capture_xcomposite_drm *cap_xcomp, AVCodecContext *video_codec_context) {
    // TODO: "/dev/dri/card0"
    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/card0", NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 1;

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    cap_xcomp->va_dpy = vactx->display;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = device_ctx; // TODO: av_buffer_ref? and in more places
    video_codec_context->hw_frames_ctx = frame_context;
    return true;
}

#define GL_COMPILE_STATUS                 0x8B81
#define GL_INFO_LOG_LENGTH                0x8B84

unsigned int esLoadShader ( gsr_capture_xcomposite_drm *cap_xcomp, unsigned int type, const char *shaderSrc ) {
   unsigned int shader;
   int compiled;
   
   // Create the shader object
   shader = cap_xcomp->egl.glCreateShader ( type );

   if ( shader == 0 )
   	return 0;

   // Load the shader source
   cap_xcomp->egl.glShaderSource ( shader, 1, &shaderSrc, NULL );
   
   // Compile the shader
   cap_xcomp->egl.glCompileShader ( shader );

   // Check the compile status
   cap_xcomp->egl.glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled ) 
   {
      int infoLen = 0;

      cap_xcomp->egl.glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );

         cap_xcomp->egl.glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
         fprintf (stderr, "Error compiling shader:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      cap_xcomp->egl.glDeleteShader ( shader );
      return 0;
   }

   return shader;

}

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82


//
///
/// \brief Load a vertex and fragment shader, create a program object, link program.
//         Errors output to log.
/// \param vertShaderSrc Vertex shader source code
/// \param fragShaderSrc Fragment shader source code
/// \return A new program object linked with the vertex/fragment shader pair, 0 on failure
//
unsigned int esLoadProgram ( gsr_capture_xcomposite_drm *cap_xcomp, const char *vertShaderSrc, const char *fragShaderSrc )
{
   unsigned int vertexShader;
   unsigned int fragmentShader;
   unsigned int programObject;
   int linked;

   // Load the vertex/fragment shaders
   vertexShader = esLoadShader ( cap_xcomp, GL_VERTEX_SHADER, vertShaderSrc );
   if ( vertexShader == 0 )
      return 0;

   fragmentShader = esLoadShader ( cap_xcomp, GL_FRAGMENT_SHADER, fragShaderSrc );
   if ( fragmentShader == 0 )
   {
      cap_xcomp->egl.glDeleteShader( vertexShader );
      return 0;
   }

   // Create the program object
   programObject = cap_xcomp->egl.glCreateProgram ( );
   
   if ( programObject == 0 )
      return 0;

   cap_xcomp->egl.glAttachShader ( programObject, vertexShader );
   cap_xcomp->egl.glAttachShader ( programObject, fragmentShader );

   // Link the program
   cap_xcomp->egl.glLinkProgram ( programObject );

   // Check the link status
   cap_xcomp->egl.glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

   if ( !linked ) 
   {
      int infoLen = 0;

      cap_xcomp->egl.glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );

         cap_xcomp->egl.glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
         fprintf (stderr, "Error linking program:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      cap_xcomp->egl.glDeleteProgram ( programObject );
      return 0;
   }

   // Free up no longer needed shader resources
   cap_xcomp->egl.glDeleteShader ( vertexShader );
   cap_xcomp->egl.glDeleteShader ( fragmentShader );

   return programObject;
}

static unsigned int LoadShadersY(gsr_capture_xcomposite_drm *cap_xcomp) {
	char vShaderStr[] =
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
		"void main()                                     \n"
		"{                                               \n"
        "  texcoords_out = texcoords;                    \n"
		"  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);   \n"
		"}                                               \n";

#if 0
	char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"


        "float imageWidth = 1920.0;\n"
        "float imageHeight = 1080.0;\n"

        "float getYPixel(vec2 position) {\n"
        "    position.y = (position.y * 2.0 / 3.0) + (1.0 / 3.0);\n"
        "    return texture2D(tex, position).x;\n"
        "}\n"
"\n"
        "vec2 mapCommon(vec2 position, float planarOffset) {\n"
        "    planarOffset += (imageWidth * floor(position.y / 2.0)) / 2.0 +\n"
        "                    floor((imageWidth - 1.0 - position.x) / 2.0);\n"
        "    float x = floor(imageWidth - 1.0 - floor(mod(planarOffset, imageWidth)));\n"
        "    float y = floor(floor(planarOffset / imageWidth));\n"
        "    return vec2((x + 0.5) / imageWidth, (y + 0.5) / (1.5 * imageHeight));\n"
        "}\n"
"\n"
        "vec2 mapU(vec2 position) {\n"
        "    float planarOffset = (imageWidth * imageHeight) / 4.0;\n"
        "    return mapCommon(position, planarOffset);\n"
        "}\n"
"\n"
        "vec2 mapV(vec2 position) {\n"
        "    return mapCommon(position, 0.0);\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"

        "vec2 pixelPosition = vec2(floor(imageWidth * texcoords_out.x),\n"
        "                        floor(imageHeight * texcoords_out.y));\n"
        "pixelPosition -= vec2(0.5, 0.5);\n"
"\n"
        "float yChannel = getYPixel(texcoords_out);\n"
        "float uChannel = texture2D(tex, mapU(pixelPosition)).x;\n"
        "float vChannel = texture2D(tex, mapV(pixelPosition)).x;\n"
        "vec4 channels = vec4(yChannel, uChannel, vChannel, 1.0);\n"
        "mat4 conversion = mat4(1.0,  0.0,    1.402, -0.701,\n"
        "                        1.0, -0.344, -0.714,  0.529,\n"
        "                        1.0,  1.772,  0.0,   -0.886,\n"
        "                        0, 0, 0, 0);\n"
        "vec3 rgb = (channels * conversion).xyz;\n"

		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#elif 1
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex1;                                    \n"
        //"uniform sampler2D tex2;                                    \n"
        "out vec4 FragColor;                                       \n"
        //"out vec4 FragColor2;                                       \n"
        "mat4 RGBtoYUV() {\n"
        "   return mat4(\n"
        "       vec4(0.257,  0.439, -0.148, 0.0),\n"
        "      vec4(0.504, -0.368, -0.291, 0.0),\n"
        "      vec4(0.098, -0.071,  0.439, 0.0),\n"
        "      vec4(0.0625, 0.500,  0.500, 1.0)\n"
        "   );\n"
        "}\n"
		"void main()                                               \n"
		"{                                                         \n"
        //"  vec3 yuv = rgb2yuv(texture(tex1, texcoords_out).rgb);             \n"
		//"  FragColor.x = yuv.x;                            \n"
        //"  FragColor2.xy = yuv.xy;                            \n"
        //" vec3 rgb = texture(tex1, texcoords_out).rgb;\n"
        "FragColor.x = (RGBtoYUV() * vec4(texture(tex1, texcoords_out).rgb, 1.0)).x;\n"
        //"FragColor2.xy = (RGBtoYUV() * vec4(texture(tex1, texcoords_out*2.0).rgb, 1.0)).zy;\n"
		"}                                                         \n";
#else
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"

        "vec3 rgb2yuv(vec3 rgb){\n"
        "    float y = 0.299*rgb.r + 0.587*rgb.g + 0.114*rgb.b;\n"
        "    return vec3(y, 0.493*(rgb.b-y), 0.877*(rgb.r-y));\n"
        "}\n"

        "vec3 yuv2rgb(vec3 yuv){\n"
        "    float y = yuv.x;\n"
        "    float u = yuv.y;\n"
        "    float v = yuv.z;\n"
        "    \n"
        "    return vec3(\n"
        "        y + 1.0/0.877*v,\n"
        "        y - 0.39393*u - 0.58081*v,\n"
        "        y + 1.0/0.493*u\n"
        "    );\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"
        "   float s = 0.5;\n"
        "    vec3 lum = texture(tex, texcoords_out).rgb;\n"
        "    vec3 chr = texture(tex, floor(texcoords_out*s-.5)/s).rgb;\n"
        "    vec3 rgb = vec3(rgb2yuv(lum).x, rgb2yuv(chr).yz);\n"
		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#endif

    unsigned int shader_program = esLoadProgram(cap_xcomp, vShaderStr, fShaderStr);
	if (shader_program == 0) {
        fprintf(stderr, "failed to create shader!\n");
        return 0;
    }

    cap_xcomp->egl.glBindAttribLocation(shader_program, 0, "pos");
    cap_xcomp->egl.glBindAttribLocation(shader_program, 1, "texcoords");
	return shader_program;
}

static unsigned int LoadShadersUV(gsr_capture_xcomposite_drm *cap_xcomp) {
	char vShaderStr[] =
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
		"void main()                                     \n"
		"{                                               \n"
        "  texcoords_out = texcoords;                    \n"
		"  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);   \n"
		"}                                               \n";

#if 0
	char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"


        "float imageWidth = 1920.0;\n"
        "float imageHeight = 1080.0;\n"

        "float getYPixel(vec2 position) {\n"
        "    position.y = (position.y * 2.0 / 3.0) + (1.0 / 3.0);\n"
        "    return texture2D(tex, position).x;\n"
        "}\n"
"\n"
        "vec2 mapCommon(vec2 position, float planarOffset) {\n"
        "    planarOffset += (imageWidth * floor(position.y / 2.0)) / 2.0 +\n"
        "                    floor((imageWidth - 1.0 - position.x) / 2.0);\n"
        "    float x = floor(imageWidth - 1.0 - floor(mod(planarOffset, imageWidth)));\n"
        "    float y = floor(floor(planarOffset / imageWidth));\n"
        "    return vec2((x + 0.5) / imageWidth, (y + 0.5) / (1.5 * imageHeight));\n"
        "}\n"
"\n"
        "vec2 mapU(vec2 position) {\n"
        "    float planarOffset = (imageWidth * imageHeight) / 4.0;\n"
        "    return mapCommon(position, planarOffset);\n"
        "}\n"
"\n"
        "vec2 mapV(vec2 position) {\n"
        "    return mapCommon(position, 0.0);\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"

        "vec2 pixelPosition = vec2(floor(imageWidth * texcoords_out.x),\n"
        "                        floor(imageHeight * texcoords_out.y));\n"
        "pixelPosition -= vec2(0.5, 0.5);\n"
"\n"
        "float yChannel = getYPixel(texcoords_out);\n"
        "float uChannel = texture2D(tex, mapU(pixelPosition)).x;\n"
        "float vChannel = texture2D(tex, mapV(pixelPosition)).x;\n"
        "vec4 channels = vec4(yChannel, uChannel, vChannel, 1.0);\n"
        "mat4 conversion = mat4(1.0,  0.0,    1.402, -0.701,\n"
        "                        1.0, -0.344, -0.714,  0.529,\n"
        "                        1.0,  1.772,  0.0,   -0.886,\n"
        "                        0, 0, 0, 0);\n"
        "vec3 rgb = (channels * conversion).xyz;\n"

		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#elif 1
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex1;                                    \n"
        //"uniform sampler2D tex2;                                    \n"
        "out vec4 FragColor;                                       \n"
        //"out vec4 FragColor2;                                       \n"
        "mat4 RGBtoYUV() {\n"
        "   return mat4(\n"
        "       vec4(0.257,  0.439, -0.148, 0.0),\n"
        "      vec4(0.504, -0.368, -0.291, 0.0),\n"
        "      vec4(0.098, -0.071,  0.439, 0.0),\n"
        "      vec4(0.0625, 0.500,  0.500, 1.0)\n"
        "   );\n"
        "}\n"
		"void main()                                               \n"
		"{                                                         \n"
        //"  vec3 yuv = rgb2yuv(texture(tex1, texcoords_out).rgb);             \n"
		//"  FragColor.x = yuv.x;                            \n"
        //"  FragColor2.xy = yuv.xy;                            \n"
        //" vec3 rgb = texture(tex1, texcoords_out).rgb;\n"
        //"FragColor.x = (RGBtoYUV() * vec4(texture(tex1, texcoords_out).rgb, 1.0)).x;\n"
        "FragColor.xy = (RGBtoYUV() * vec4(texture(tex1, texcoords_out*2.0).rgb, 1.0)).zy;\n"
		"}                                                         \n";
#else
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"

        "vec3 rgb2yuv(vec3 rgb){\n"
        "    float y = 0.299*rgb.r + 0.587*rgb.g + 0.114*rgb.b;\n"
        "    return vec3(y, 0.493*(rgb.b-y), 0.877*(rgb.r-y));\n"
        "}\n"

        "vec3 yuv2rgb(vec3 yuv){\n"
        "    float y = yuv.x;\n"
        "    float u = yuv.y;\n"
        "    float v = yuv.z;\n"
        "    \n"
        "    return vec3(\n"
        "        y + 1.0/0.877*v,\n"
        "        y - 0.39393*u - 0.58081*v,\n"
        "        y + 1.0/0.493*u\n"
        "    );\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"
        "   float s = 0.5;\n"
        "    vec3 lum = texture(tex, texcoords_out).rgb;\n"
        "    vec3 chr = texture(tex, floor(texcoords_out*s-.5)/s).rgb;\n"
        "    vec3 rgb = vec3(rgb2yuv(lum).x, rgb2yuv(chr).yz);\n"
		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#endif

    unsigned int shader_program = esLoadProgram(cap_xcomp, vShaderStr, fShaderStr);
	if (shader_program == 0) {
        fprintf(stderr, "failed to create shader!\n");
        return 0;
    }

    cap_xcomp->egl.glBindAttribLocation(shader_program, 0, "pos");
    cap_xcomp->egl.glBindAttribLocation(shader_program, 1, "texcoords");
	return shader_program;
}

#define GL_FLOAT				0x1406
#define GL_FALSE				0
#define GL_TRUE					1
#define GL_TRIANGLES				0x0004
#define DRM_FORMAT_MOD_INVALID 72057594037927935

#define EGL_TRUE                          1
#define EGL_IMAGE_PRESERVED_KHR           0x30D2
#define EGL_NATIVE_PIXMAP_KHR             0x30B0

static uint32_t fourcc(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (d << 24) | (c << 16) | (b << 8) | a;
}

static int gsr_capture_xcomposite_drm_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->params.window, &attr)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);
    Window c;
    XTranslateCoordinates(cap_xcomp->dpy, cap_xcomp->params.window, DefaultRootWindow(cap_xcomp->dpy), 0, 0, &cap_xcomp->window_pos.x, &cap_xcomp->window_pos.y, &c);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(cap_xcomp->dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!gsr_egl_load(&cap_xcomp->egl, cap_xcomp->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed to load opengl\n");
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: could not find eglExportDMABUFImageQueryMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: could not find eglExportDMABUFImageMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    if(!gsr_vaapi_load(&cap_xcomp->vaapi)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed to load vaapi\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    /* Disable vsync */
    cap_xcomp->egl.eglSwapInterval(cap_xcomp->egl.egl_display, 0);
#if 0
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->gl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    cap_xcomp->target_texture_id = gl_create_texture(cap_xcomp, cap_xcomp->texture_size.x, cap_xcomp->texture_size.y);
    if(cap_xcomp->target_texture_id == 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to create opengl texture\n");
        gsr_capture_xcomposite_stop(cap, video_codec_context);
        return -1;
    }

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
#else
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->egl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    {
        const intptr_t pixmap_attrs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_NONE,
        };

        EGLImage img = cap_xcomp->egl.eglCreateImage(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_context, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uint64_t)window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), pixmap_attrs);
        if(!img) {
            fprintf(stderr, "eglCreateImage failed\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->fourcc, &cap_xcomp->num_planes, &cap_xcomp->modifiers)) {
            fprintf(stderr, "eglExportDMABUFImageQueryMESA failed\n"); 
            return -1;
        }

        if(cap_xcomp->num_planes != 1) {
            // TODO: FAIL!
            fprintf(stderr, "Blablalba\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->dmabuf_fd, &cap_xcomp->stride, &cap_xcomp->offset)) {
            fprintf(stderr, "eglExportDMABUFImageMESA failed\n");
            return -1;
        }

        fprintf(stderr, "texture: %u, dmabuf: %d, stride: %d, offset: %d\n", window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), cap_xcomp->dmabuf_fd, cap_xcomp->stride, cap_xcomp->offset);
        fprintf(stderr, "fourcc: %d, num planes: %d, modifiers: %zu\n", cap_xcomp->fourcc, cap_xcomp->num_planes, cap_xcomp->modifiers);
    }

    if(!drm_create_codec_context(cap_xcomp, video_codec_context)) {
        fprintf(stderr, "failed to create hw codec context\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    //fprintf(stderr, "sneed: %u\n", cap_xcomp->FramebufferName);
    return 0;
#endif
}

static void gsr_capture_xcomposite_drm_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_xcomp->created_hw_frame) {
        cap_xcomp->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: failed to allocate frame\n");
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = AVCOL_RANGE_JPEG;

        int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0);
        if(res < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: av_hwframe_get_buffer failed 1: %d\n", res);
            return;
        }

        fprintf(stderr, "fourcc: %u\n", cap_xcomp->fourcc);
        fprintf(stderr, "va surface id: %u\n", (VASurfaceID)(uintptr_t)(*frame)->data[3]);

        VADRMPRIMESurfaceDescriptor prime;

        VASurfaceID surface_id = (uintptr_t)(*frame)->data[3];
        VAStatus va_status = cap_xcomp->vaapi.vaExportSurfaceHandle(cap_xcomp->va_dpy, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime); // TODO: Composed layers
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaExportSurfaceHandle failed\n");
            return;
        }
        cap_xcomp->vaapi.vaSyncSurface(cap_xcomp->va_dpy, surface_id);

        fprintf(stderr, "fourcc: %u, width: %u, height: %u\n", prime.fourcc, prime.width, prime.height);
        for(int i = 0; i < prime.num_layers; ++i) {
            fprintf(stderr, "  drm format: %u, num planes: %u\n", prime.layers[i].drm_format, prime.layers[i].num_planes);
            for(int j = 0; j < prime.layers[i].num_planes; ++j) {
                const uint32_t object_index = prime.layers[i].object_index[j];
                fprintf(stderr, "    object index: %u, offset: %u, pitch: %u, fd: %d, size: %u, drm format mod: %lu\n", object_index, prime.layers[i].offset[j], prime.layers[i].pitch[j], prime.objects[object_index].fd, prime.objects[object_index].size, prime.objects[object_index].drm_format_modifier);
            }
        }

        #define EGL_LINUX_DRM_FOURCC_EXT          0x3271
        #define EGL_WIDTH                         0x3057
        #define EGL_HEIGHT                        0x3056
        #define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
        #define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
        #define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
        #define EGL_LINUX_DMA_BUF_EXT             0x3270

        #define GL_TEXTURE0				0x84C0
        #define GL_COLOR_ATTACHMENT1              0x8CE1

        #define FOURCC_NV12 842094158

        if(prime.fourcc == FOURCC_NV12) { // This happens on AMD
            while(cap_xcomp->egl.glGetError()) {}
            while(cap_xcomp->egl.eglGetError() != EGL_SUCCESS){}

            EGLImage images[2];
            cap_xcomp->egl.glGenTextures(2, cap_xcomp->target_textures);
            assert(cap_xcomp->egl.glGetError() == 0);
            for(int i = 0; i < 2; ++i) {
                const uint32_t formats[2] = { fourcc('R', '8', ' ', ' '), fourcc('G', 'R', '8', '8') };
                const int layer = i;
                const int plane = 0;

                const intptr_t img_attr[] = {
                    EGL_LINUX_DRM_FOURCC_EXT,   formats[i],
                    EGL_WIDTH,                  prime.width / (1 + i), // half size
                    EGL_HEIGHT,                 prime.height / (1 + i), // for chroma
                    EGL_DMA_BUF_PLANE0_FD_EXT,  prime.objects[prime.layers[layer].object_index[plane]].fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,  prime.layers[layer].offset[plane],
                    EGL_DMA_BUF_PLANE0_PITCH_EXT,  prime.layers[layer].pitch[plane],
                    EGL_NONE
                };
                images[i] = cap_xcomp->egl.eglCreateImage(cap_xcomp->egl.egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr); // TODO: Cleanup at the end of this for loop
                assert(images[i]);
                assert(cap_xcomp->egl.eglGetError() == EGL_SUCCESS);

                //cap_xcomp->egl.glActiveTexture(GL_TEXTURE0 + i);
                cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, cap_xcomp->target_textures[i]);
                assert(cap_xcomp->egl.glGetError() == 0);

                cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                assert(cap_xcomp->egl.glGetError() == 0);

                cap_xcomp->egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images[i]);
                assert(cap_xcomp->egl.glGetError() == 0);
                assert(cap_xcomp->egl.eglGetError() == EGL_SUCCESS);
            }
            //cap_xcomp->egl.glActiveTexture(GL_TEXTURE0);
            cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);



            cap_xcomp->egl.glGenFramebuffers(1, &cap_xcomp->FramebufferNameY);
            cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferNameY);

            cap_xcomp->egl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cap_xcomp->target_textures[0], 0);
           // cap_xcomp->egl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, cap_xcomp->target_textures[1], 0);

            // Set the list of draw buffers.
            unsigned int DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
            cap_xcomp->egl.glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers

            if(cap_xcomp->egl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "Failed to setup framebuffer\n");
                return;
            }

            cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, 0);

            cap_xcomp->egl.glGenFramebuffers(1, &cap_xcomp->FramebufferNameUV);
            cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferNameUV);

            cap_xcomp->egl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cap_xcomp->target_textures[1], 0);
           // cap_xcomp->egl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, cap_xcomp->target_textures[1], 0);

            // Set the list of draw buffers.
            cap_xcomp->egl.glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers

            if(cap_xcomp->egl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "Failed to setup framebuffer\n");
                return;
            }

            cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, 0);

            //cap_xcomp->egl.glGenVertexArrays(1, &cap_xcomp->quad_VertexArrayID);
            //cap_xcomp->egl.glBindVertexArray(cap_xcomp->quad_VertexArrayID);

            static const float g_quad_vertex_buffer_data[] = {
                -1.0f, -1.0f, 0.0f,
                1.0f, -1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f,
                1.0f, -1.0f, 0.0f,
                1.0f,  1.0f, 0.0f,
            };

            //cap_xcomp->egl.glGenBuffers(1, &cap_xcomp->quad_vertexbuffer);
            //cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, cap_xcomp->quad_vertexbuffer);
            //cap_xcomp->egl.glBufferData(GL_ARRAY_BUFFER, sizeof(g_quad_vertex_buffer_data), g_quad_vertex_buffer_data, GL_STATIC_DRAW);

            // Create and compile our GLSL program from the shaders
            cap_xcomp->shader_y = LoadShadersY(cap_xcomp);
            cap_xcomp->shader_uv = LoadShadersUV(cap_xcomp);
            //int tex1 = cap_xcomp->egl.glGetUniformLocation(cap_xcomp->shader_y, "tex1");
            //cap_xcomp->egl.glUniform1i(tex1, 0);
            //tex1 = cap_xcomp->egl.glGetUniformLocation(cap_xcomp->shader_uv, "tex1");
            //cap_xcomp->egl.glUniform1i(tex1, 0);
            //int tex2 = cap_xcomp->egl.glGetUniformLocation(shader_program, "tex2");
            //fprintf(stderr, "uniform id: %u\n", tex1);

            float vVertices[] = {
                -1.0f,  1.0f,  0.0f, 1.0f,
                -1.0f, -1.0f,  0.0f, 0.0f,
                1.0f, -1.0f,  1.0f, 0.0f,

                -1.0f,  1.0f,  0.0f, 1.0f,
                1.0f, -1.0f,  1.0f, 0.0f,
                1.0f,  1.0f,  1.0f, 1.0f
            };

            unsigned int quadVBO;
            cap_xcomp->egl.glGenVertexArrays(1, &cap_xcomp->quadVAO);
            cap_xcomp->egl.glGenBuffers(1, &quadVBO);
            cap_xcomp->egl.glBindVertexArray(cap_xcomp->quadVAO);
            cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            cap_xcomp->egl.glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices), &vVertices, GL_STATIC_DRAW);

            cap_xcomp->egl.glEnableVertexAttribArray(0);
            cap_xcomp->egl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

            cap_xcomp->egl.glEnableVertexAttribArray(1);
            cap_xcomp->egl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            cap_xcomp->egl.glBindVertexArray(0);

            //cap_xcomp->egl.glUniform1i(tex1, 0);
            //cap_xcomp->egl.glUniform1i(tex2, 1);

            //cap_xcomp->egl.glViewport(0, 0, 1920, 1080);

            //cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, 0);
            //cap_xcomp->egl.glBindVertexArray(0);
        } else { // This happens on intel
            fprintf(stderr, "unexpected fourcc: %u, expected nv12\n", prime.fourcc);
            abort();
        }

        // Clear texture with black background because the source texture (window_texture_get_opengl_texture_id(&cap_xcomp->window_texture))
        // might be smaller than cap_xcomp->target_texture_id
        // TODO:
        //cap_xcomp->egl.glClearTexImage(cap_xcomp->target_texture_id, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
}

static bool gsr_capture_xcomposite_drm_should_stop(gsr_capture *cap, bool *err) {
    return false;
}

#define GL_FLOAT				0x1406
#define GL_FALSE				0
#define GL_TRUE					1
#define GL_TRIANGLES				0x0004

static int gsr_capture_xcomposite_drm_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;
    vec2i source_size = cap_xcomp->texture_size;

    cap_xcomp->egl.glBindVertexArray(cap_xcomp->quadVAO);
    cap_xcomp->egl.glViewport(0, 0, source_size.x, source_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));

    {
        cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferNameY);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

        cap_xcomp->egl.glUseProgram(cap_xcomp->shader_y);
        cap_xcomp->egl.glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    {
        cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferNameUV);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

        cap_xcomp->egl.glUseProgram(cap_xcomp->shader_uv);
        cap_xcomp->egl.glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    cap_xcomp->egl.glBindVertexArray(0);
    cap_xcomp->egl.glUseProgram(0);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);
    cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    cap_xcomp->egl.eglSwapBuffers(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_surface);

    return 0;
}

static void gsr_capture_xcomposite_drm_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;
    if(cap->priv) {
        free(cap->priv);
        cap->priv = NULL;
    }
    if(cap_xcomp->dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(cap_xcomp->dpy);
        cap_xcomp->dpy = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_drm_create(const gsr_capture_xcomposite_drm_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_drm *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_drm));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_xcomp);
        return NULL;
    }

    cap_xcomp->dpy = display;
    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_drm_start,
        .tick = gsr_capture_xcomposite_drm_tick,
        .should_stop = gsr_capture_xcomposite_drm_should_stop,
        .capture = gsr_capture_xcomposite_drm_capture,
        .destroy = gsr_capture_xcomposite_drm_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
