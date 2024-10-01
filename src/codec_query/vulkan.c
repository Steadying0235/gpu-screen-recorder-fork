#include "../../include/codec_query/vulkan.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>

#define MAX_PHYSICAL_DEVICES 32

static const char *required_device_extensions[] = {
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_video_encode_queue",
    "VK_KHR_video_queue",
    "VK_KHR_video_maintenance1",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_external_memory_host",
    "VK_EXT_image_drm_format_modifier"
};
static int num_required_device_extensions = 8;

bool gsr_get_supported_video_codecs_vulkan(gsr_supported_video_codecs *video_codecs, const char *card_path, bool cleanup) {
    memset(video_codecs, 0, sizeof(*video_codecs));
#if 0
    bool success = false;
    VkInstance instance = NULL;
    VkPhysicalDevice physical_devices[MAX_PHYSICAL_DEVICES];
    VkDevice device = NULL;
    VkExtensionProperties *device_extensions = NULL;

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GPU Screen Recorder",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "GPU Screen Recorder",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    const VkInstanceCreateInfo instance_create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info
    };

    if(vkCreateInstance(&instance_create_info, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkCreateInstance failed\n");
        goto done;
    }

    uint32_t num_devices = 0;
    if(vkEnumeratePhysicalDevices(instance, &num_devices, NULL) != VK_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkEnumeratePhysicalDevices (query num devices) failed\n");
        goto done;
    }

    if(num_devices == 0) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: no vulkan capable device found\n");
        goto done;
    }

    if(num_devices > MAX_PHYSICAL_DEVICES)
        num_devices = MAX_PHYSICAL_DEVICES;
    
    if(vkEnumeratePhysicalDevices(instance, &num_devices, physical_devices) != VK_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkEnumeratePhysicalDevices (get data) failed\n");
        goto done;
    }

    VkPhysicalDevice physical_device = NULL;
    char device_card_path[128];
    for(uint32_t i = 0; i < num_devices; ++i) {
        VkPhysicalDeviceDrmPropertiesEXT device_drm_properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT
        };

        VkPhysicalDeviceProperties2 device_properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &device_drm_properties
        };
        vkGetPhysicalDeviceProperties2(physical_devices[i], &device_properties);

        if(!device_drm_properties.hasPrimary)
            continue;

        snprintf(device_card_path, sizeof(device_card_path), DRM_DEV_NAME, DRM_DIR_NAME, (int)device_drm_properties.primaryMinor);
        if(strcmp(device_card_path, card_path) == 0) {
            physical_device = physical_devices[i];
            break;
        }
    }

    if(!physical_device) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: failed to find a vulkan device that matches opengl device %s\n", card_path);
        goto done;
    }

    const VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .enabledExtensionCount = num_required_device_extensions,
        .ppEnabledExtensionNames = required_device_extensions
    };

    if(vkCreateDevice(physical_device, &device_create_info, NULL, &device) != VK_SUCCESS) {
        //fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkCreateDevice failed. Device %s likely doesn't support vulkan video encoding\n", card_path);
        goto done;
    }

    uint32_t num_device_extensions = 0;
    if(vkEnumerateDeviceExtensionProperties(physical_device, NULL, &num_device_extensions, NULL) != VK_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkEnumerateDeviceExtensionProperties (query num device extensions) failed\n");
        goto done;
    }

    device_extensions = calloc(num_device_extensions, sizeof(VkExtensionProperties));
    if(!device_extensions) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: failed to allocate %d device extensions\n", num_device_extensions);
        goto done;
    }

    if(vkEnumerateDeviceExtensionProperties(physical_device, NULL, &num_device_extensions, device_extensions) != VK_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_get_supported_video_codecs_vulkan: vkEnumerateDeviceExtensionProperties (get data) failed\n");
        goto done;
    }

    for(uint32_t i = 0; i < num_device_extensions; ++i) {
        if(strcmp(device_extensions[i].extensionName, "VK_KHR_video_encode_h264") == 0) {
            video_codecs->h264 = true;
        } else if(strcmp(device_extensions[i].extensionName, "VK_KHR_video_encode_h265") == 0) {
            // TODO: Verify if 10bit and hdr are actually supported
            video_codecs->hevc = true;
            video_codecs->hevc_10bit = true;
            video_codecs->hevc_hdr = true;
        }
    }

    success = true;

    done:
    if(cleanup) {
        if(device)
            vkDestroyDevice(device, NULL);
        if(instance)
            vkDestroyInstance(instance, NULL);
    }
    if(device_extensions)
        free(device_extensions);
    return success;
#else
    // TODO: Low power query
    video_codecs->h264 = (gsr_supported_video_codec){ true, false };
    return true;
#endif
}
