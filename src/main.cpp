#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string>
#include <vector>

#define GLX_GLXEXT_PROTOTYPES
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

extern "C" {
#include <libavutil/hwcontext_cuda.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}
#include <cudaGL.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

//#include <CL/cl.h>

struct ScopedGLXFBConfig {
    ~ScopedGLXFBConfig() {
        if(configs)
            XFree(configs);
    }

    GLXFBConfig *configs = nullptr;
};

struct WindowPixmap {
    WindowPixmap() : pixmap(None), glx_pixmap(None), texture_id(0), target_texture_id(0), texture_width(0), texture_height(0) {

    }

    Pixmap pixmap;
    GLXPixmap glx_pixmap;
    GLuint texture_id;
    GLuint target_texture_id;

    GLint texture_width;
    GLint texture_height;
};

static bool x11_supports_composite_named_window_pixmap(Display *dpy) {
    int extension_major;
    int extension_minor;
    if(!XCompositeQueryExtension(dpy, &extension_major, &extension_minor))
        return false;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(dpy, &major_version, &minor_version) && (major_version > 0 || minor_version >= 2);
}

static void cleanup_window_pixmap(Display *dpy, WindowPixmap &pixmap) {
    if(pixmap.target_texture_id) {
        glDeleteTextures(1, &pixmap.target_texture_id);
        pixmap.target_texture_id = 0;
    }

    if(pixmap.texture_id) {
        glDeleteTextures(1, &pixmap.texture_id);
        pixmap.texture_id = 0;
        pixmap.texture_width = 0;
        pixmap.texture_height = 0;
    }

    if(pixmap.glx_pixmap) {
        glXReleaseTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT);
		glXDestroyPixmap(dpy, pixmap.glx_pixmap);
        pixmap.glx_pixmap = None;
    }

    if(pixmap.pixmap) {
        XFreePixmap(dpy, pixmap.pixmap);
        pixmap.pixmap = None;
    }
}

static bool recreate_window_pixmap(Display *dpy, Window window_id, WindowPixmap &pixmap) {
    cleanup_window_pixmap(dpy, pixmap);
    
    const int pixmap_config[] = {
		GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
		GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
		GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
		GLX_BIND_TO_MIPMAP_TEXTURE_EXT, True,
		GLX_DOUBLEBUFFER, False,
		//GLX_Y_INVERTED_EXT, (int)GLX_DONT_CARE,
		None
    };

    // Note that mipmap is generated even though its not used.
    // glCopyImageSubData fails if the texture doesn't have mipmap.
    const int pixmap_attribs[] = {
		GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
		GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
		GLX_MIPMAP_TEXTURE_EXT, 1,
		None
    };

	int c;
	GLXFBConfig *configs = glXChooseFBConfig(dpy, 0, pixmap_config, &c);
	if(!configs) {
		fprintf(stderr, "Failed too choose fb config\n");
		return false;
	}
    ScopedGLXFBConfig scoped_configs;
    scoped_configs.configs = configs;

	Pixmap new_window_pixmap = XCompositeNameWindowPixmap(dpy, window_id);
	if(!new_window_pixmap) {
		fprintf(stderr, "Failed to get pixmap for window %ld\n", window_id);
		return false;
	}

	GLXPixmap glx_pixmap = glXCreatePixmap(dpy, *configs, new_window_pixmap, pixmap_attribs);
	if(!glx_pixmap) {
		fprintf(stderr, "Failed to create glx pixmap\n");
        XFreePixmap(dpy, new_window_pixmap);
		return false;
	}

    pixmap.pixmap = new_window_pixmap;
    pixmap.glx_pixmap = glx_pixmap;

    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &pixmap.texture_id);
	glBindTexture(GL_TEXTURE_2D, pixmap.texture_id);

	//glEnable(GL_BLEND);
    //glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &pixmap.texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &pixmap.texture_height);
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);//GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    printf("texture width: %d, height: %d\n", pixmap.texture_width, pixmap.texture_height);

    // Generating this second texture is needed because cuGraphicsGLRegisterImage
    // cant be used with the texture that is mapped directly to the pixmap.
    // TODO: Investigate if it's somehow possible to use the pixmap texture directly,
    // this should improve performance since only less image copy is then needed every frame.
    glGenTextures(1, &pixmap.target_texture_id);
    glBindTexture(GL_TEXTURE_2D, pixmap.target_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixmap.texture_width, pixmap.texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    int err2 = glGetError();
    printf("error: %d\n", err2);
    glCopyImageSubData(
        pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
        pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
        pixmap.texture_width, pixmap.texture_height, 1);
    int err = glGetError();
    printf("error: %d\n", err);
    //glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL); 
	//glGenerateTextureMipmapEXT(glxpixmap, GL_TEXTURE_2D);

	//glGenerateMipmap(GL_TEXTURE_2D);

	//glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	//glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );


	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);//GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBindTexture(GL_TEXTURE_2D, 0);
    
    return pixmap.texture_id != 0 && pixmap.target_texture_id != 0;
}

std::vector<std::string> get_hardware_acceleration_device_names() {
    int iGpu = 0;
    int nGpu = 0;
    cuDeviceGetCount(&nGpu);
    if(iGpu < 0 || iGpu >= nGpu) {
        fprintf(stderr, "Error: failed...\n");
        return {};
    }

    CUdevice cuDevice = 0;
    cuDeviceGet(&cuDevice, iGpu);
    char deviceName[80];
    cuDeviceGetName(deviceName, sizeof(deviceName), cuDevice);
    printf("device name: %s\n", deviceName);
    return { deviceName };
}

static void receive_frames(AVCodecContext *av_codec_context, AVStream *stream, AVFormatContext *av_format_context) {
    AVPacket av_packet;
    av_init_packet(&av_packet);
    for( ; ; ) {
        av_packet.data = NULL;
        av_packet.size = 0;
		int res = avcodec_receive_packet(av_codec_context, &av_packet);
		if(res == 0) { // we have a packet, send the packet to the muxer
            av_packet_rescale_ts(&av_packet, av_codec_context->time_base, stream->time_base);
            av_packet.stream_index = stream->index;
            if(av_write_frame(av_format_context, &av_packet) < 0) {
                fprintf(stderr, "Error: Failed to write frame to muxer\n");
            }
            //av_packet_unref(&av_packet);
		} else if(res == AVERROR(EAGAIN)) { // we have no packet
            //printf("No packet!\n");
			break;
		} else if(res == AVERROR_EOF) { // this is the end of the stream
			printf("End of stream!\n");
            break;
		} else {
			printf("Unexpected error: %d\n", res);
            break;
		}
	}
    av_packet_unref(&av_packet);
}

static AVStream* add_stream(AVFormatContext *av_format_context, AVCodec **codec, enum AVCodecID codec_id, const WindowPixmap &window_pixmap) {
    //*codec = avcodec_find_encoder(codec_id);
    *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if(!*codec) {
        *codec = avcodec_find_encoder_by_name("nvenc_h264");
    }
    if(!*codec) {
        fprintf(stderr, "Error: Could not find h264_nvenc or nvenc_h264 encoder for %s\n", avcodec_get_name(codec_id));
        exit(1);
    }

    AVStream *stream = avformat_new_stream(av_format_context, *codec);
    if(!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    AVCodecContext *codec_context = stream->codec;

    switch((*codec)->type) {
        case AVMEDIA_TYPE_AUDIO: {
            codec_context->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            codec_context->bit_rate = 64000;
            codec_context->sample_rate = 44100;
            codec_context->channels = 2;
            break;
        }
        case AVMEDIA_TYPE_VIDEO: {
            codec_context->codec_id = codec_id;
            codec_context->bit_rate = 8000000;
            // Resolution must be a multiple of two
            codec_context->width = window_pixmap.texture_width & ~1;
            codec_context->height = window_pixmap.texture_height & ~1;
            // Timebase: This is the fundamental unit of time (in seconds) in terms of
            // which frame timestamps are represented. For fixed-fps content,
            // timebase should be 1/framerate and timestamp increments should be identical to 1
            codec_context->time_base.num = 1;
            codec_context->time_base.den = 60;
            //codec_context->framerate.num = 60;
            //codec_context->framerate.den = 1;
            codec_context->sample_aspect_ratio.num = 1;
            codec_context->sample_aspect_ratio.den = 1;
            codec_context->gop_size = 12; // Emit one intra frame every twelve frames at most
            codec_context->pix_fmt = AV_PIX_FMT_CUDA;
            if(codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
                codec_context->mb_decision = 2;

            //stream->time_base = codec_context->time_base;
            //codec_context->ticks_per_frame = 30;
            break;
        }
        default:
            break;
    }

    // Some formats want stream headers to be seperate
    if(av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return stream;
}

static void open_video(AVCodec *codec, AVStream *stream, WindowPixmap &window_pixmap, AVBufferRef **device_ctx, CUgraphicsResource *cuda_graphics_resource) {
    int ret;
    AVCodecContext *codec_context = stream->codec;

    std::vector<std::string> hardware_accelerated_devices = get_hardware_acceleration_device_names();
    if(hardware_accelerated_devices.empty()) {
        fprintf(stderr, "Error: No hardware accelerated device was found on your system\n");
        exit(1);
    }

    if(av_hwdevice_ctx_create(device_ctx, AV_HWDEVICE_TYPE_CUDA, hardware_accelerated_devices[0].c_str(), NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context for gpu: %s\n", hardware_accelerated_devices[0].c_str());
        exit(1);
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(*device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        exit(1);
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = codec_context->width;
    hw_frame_context->height = codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_0BGR32;
    hw_frame_context->format = codec_context->pix_fmt;
    hw_frame_context->device_ref = *device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)(*device_ctx)->data;

    if(av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context (note: ffmpeg version needs to be > 4.0\n");
        exit(1);
    }

    codec_context->hw_device_ctx = *device_ctx;
    codec_context->hw_frames_ctx = frame_context;

    ret = avcodec_open2(codec_context, codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n", "blabla");//av_err2str(ret));
        exit(1);
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)(*device_ctx)->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if(!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    CUresult res;
    CUcontext old_ctx;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);
    res = cuGraphicsGLRegisterImage(cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    //cuGraphicsUnregisterResource(*cuda_graphics_resource);
    if(res != CUDA_SUCCESS) {
        fprintf(stderr, "Error: cuGraphicsGLRegisterImage failed, error %d, texture id: %u\n", res, window_pixmap.target_texture_id);
        exit(1);
    }
    res = cuCtxPopCurrent(&old_ctx);
}

static void close_video(AVStream *video_stream, AVFrame *frame) {
    //avcodec_close(video_stream->codec);
    //av_frame_free(&frame);
}

int main(int argc, char **argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: hardware-screen-recorder <window_id>\n");
        return 1;
    }

    Window src_window_id = strtol(argv[1], nullptr, 0);

    Display *dpy = XOpenDisplay(nullptr);
    if(!dpy) {
        fprintf(stderr, "Error: Failed to open display\n");
        return 1;
    }

    bool has_name_pixmap = x11_supports_composite_named_window_pixmap(dpy);
    if(!has_name_pixmap) {
        fprintf(stderr, "Error: XCompositeNameWindowPixmap is not supported by your X11 server\n");
        return 1;
    }
    
    // TODO: Verify if this is needed
    int screen_count = ScreenCount(dpy);
    for(int i = 0; i < screen_count; ++i) {
        XCompositeRedirectSubwindows(dpy, RootWindow(dpy, i), CompositeRedirectAutomatic);
    }

    XWindowAttributes attr;
    if(!XGetWindowAttributes(dpy, src_window_id, &attr)) {
        fprintf(stderr, "Error: Invalid window id: %lu\n", src_window_id);
        return 1;
    }

    //glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
    if(!glfwInit()) {
        fprintf(stderr, "Error: Failed to initialize glfw\n");
        return 1;
    }

    glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "Hello world", nullptr, nullptr);
    if(!window) {
        fprintf(stderr, "Error: Failed to create glfw window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    glewExperimental = GL_TRUE;
	GLenum nGlewError = glewInit();
	if (nGlewError != GLEW_OK) {
		fprintf(stderr, "%s - Error initializing GLEW! %s\n", __FUNCTION__, glewGetErrorString(nGlewError));
		return 1;
	}
	glGetError(); // to clear the error caused deep in GLEW

    WindowPixmap window_pixmap;
    if(!recreate_window_pixmap(dpy, src_window_id, window_pixmap)) {
        fprintf(stderr, "Error: Failed to create glx pixmap for window: %lu\n", src_window_id);
        return 1;
    }

    const char *filename = "test_video.mp4";


    // Video start
    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, nullptr, filename);
    if(!av_format_context) {
        fprintf(stderr, "Error: Failed to deduce output format from file extension .mp4\n");
        return 1;
    }

    AVOutputFormat *output_format = av_format_context->oformat;
    AVCodec *video_codec;
    AVStream *video_stream = add_stream(av_format_context, &video_codec, output_format->video_codec, window_pixmap);
    if(!video_stream) {
        fprintf(stderr, "Error: Failed to create video stream\n");
        return 1;
    }

    if(cuInit(0) < 0) {
        fprintf(stderr, "Error: cuInit failed\n");
        return {};
    }

    AVBufferRef *device_ctx;
    CUgraphicsResource cuda_graphics_resource;
    open_video(video_codec, video_stream, window_pixmap, &device_ctx, &cuda_graphics_resource);
    av_dump_format(av_format_context, 0, filename, 1);

    if(!(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if(ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, "blabla");//av_err2str(ret));
            return 1;
        }
    }

    int ret = avformat_write_header(av_format_context, nullptr);
    if(ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n", "blabla");//av_err2str(ret));
        return 1;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if(!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    //av_frame_free(&rgb_frame);
    //avcodec_close(av_codec_context);

    XSelectInput(dpy, src_window_id, StructureNotifyMask);

    int damage_event;
    int damage_error;
    if(!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "Error: XDamage is not supported by your X11 server\n");
        return 1;
    }

    Damage xdamage = XDamageCreate(dpy, src_window_id, XDamageReportNonEmpty);

    int frame_count = 0;

    CUresult res;
    CUcontext old_ctx;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);

    // Get texture
    res = cuGraphicsResourceSetMapFlags(cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

    // Map texture to cuda array
    CUarray mapped_array;
    res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

    // Release texture
    //res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    double start_time = glfwGetTime();
    double frame_timer_start = start_time;
    int fps = 0;
    int current_fps = 30;

    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    frame->format = video_stream->codec->pix_fmt;
    frame->width = video_stream->codec->width;
    frame->height = video_stream->codec->height;

    if(av_hwframe_get_buffer(video_stream->codec->hw_frames_ctx, frame, 0) < 0) {
        fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
        exit(1);
    }

    XEvent e;
    while(!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();

        if (XCheckTypedEvent(dpy, ConfigureNotify, &e)) {
            // Window resize
            printf("Resize window!\n");
            recreate_window_pixmap(dpy, src_window_id, window_pixmap);
        }

        if (XCheckTypedEvent(dpy, damage_event + XDamageNotify, &e)) {
            //printf("Redraw!\n");
            XDamageNotifyEvent *de = (XDamageNotifyEvent*)&e;
            // de->drawable is the window ID of the damaged window
            XserverRegion region = XFixesCreateRegion(dpy, nullptr, 0);
            // Subtract all the damage, repairing the window
            XDamageSubtract(dpy, de->damage, None, region);
            XFixesDestroyRegion(dpy, region);

            // TODO: Use a framebuffer instead. glCopyImageSubData requires opengl 4.2
            glCopyImageSubData(
                window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                window_pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                window_pixmap.texture_width, window_pixmap.texture_height, 1);
            //int err = glGetError();
            //printf("error: %d\n", err);

            CUDA_MEMCPY2D memcpy_struct;
            memcpy_struct.srcXInBytes = 0;
            memcpy_struct.srcY = 0;
            memcpy_struct.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;
            
            memcpy_struct.dstXInBytes = 0;
            memcpy_struct.dstY = 0;
            memcpy_struct.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

            memcpy_struct.srcArray = mapped_array;
            memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
            memcpy_struct.dstPitch = frame->linesize[0];
            memcpy_struct.WidthInBytes = frame->width * 4;
            memcpy_struct.Height = frame->height;
            cuMemcpy2D(&memcpy_struct);
            //res = cuCtxPopCurrent(&old_ctx);
        }

        ++fps;

        const double target_fps = 0.0166666666;

        double time_now = glfwGetTime();
        double frame_timer_elapsed = time_now - frame_timer_start;
        double elapsed = time_now - start_time;
        if(elapsed >= 1.0) {
            printf("fps: %d\n", fps);
            start_time = time_now;
            current_fps = fps;
            fps = 0;
        }

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if(frame_time_overflow >= 0.0) {
            frame_timer_start = time_now - frame_time_overflow;
            frame->pts = frame_count;
            frame_count += 1;
            if(avcodec_send_frame(video_stream->codec, frame) < 0) {
                fprintf(stderr, "Error: avcodec_send_frame failed\n");
            } else {
                receive_frames(video_stream->codec, video_stream, av_format_context);
            }
        }

        //av_frame_free(&frame);
    }

    if(av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    //close_video(video_stream, NULL);

   // if(!(output_format->flags & AVFMT_NOFILE))
    //    avio_close(av_format_context->pb);
    //avformat_free_context(av_format_context);
    //XDamageDestroy(dpy, xdamage);

    //cleanup_window_pixmap(dpy, window_pixmap);
    for(int i = 0; i < screen_count; ++i) {
        XCompositeUnredirectSubwindows(dpy, RootWindow(dpy, i), CompositeRedirectAutomatic);
    }
    XCloseDisplay(dpy);
}
