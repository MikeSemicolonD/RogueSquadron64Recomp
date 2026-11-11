#include "video/vr_openxr.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plume_vulkan.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "hle/rt64_application.h"
#include "hle/rt64_rs64_vr.h"
#include "util/debug_logs.h"
#include "video/video_config.h"

using recomp::dbg::env_on;
using recomp::dbg::env_str;

namespace {

static constexpr const char* kAppName = "Rogue Squadron 64 Recompiled";
static constexpr int64_t kVkFormatR8G8B8A8Srgb = 43;
static constexpr uint32_t kNotAcquired = UINT32_MAX;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<std::unique_ptr<plume::RenderTexture>> images;
    uint32_t acquired = kNotAcquired;
    bool released = false;
};

struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    EyeSwapchain eyes[2];
    std::mutex* queueMutex = nullptr;
    bool frameBegun = false;
    XrTime displayTime = 0;
    std::atomic<int> running{ 0 };
    std::atomic<int64_t> predictedTime{ 0 };
    std::atomic<int64_t> predictedPeriod{ 0 };
    PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR getGraphicsDevice = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
};

State g;

template <typename T>
bool load_function(const char* name, T& fn)
{

    return XR_SUCCEEDED(xrGetInstanceProcAddr(g.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) && (fn != nullptr);
}

// The runtime lists extensions as one space-separated string.
template <typename Fn>
std::vector<std::string> extension_list(Fn fn)
{

    uint32_t size = 0;

    if (XR_FAILED(fn(g.instance, g.system, 0, &size, nullptr)) || (size == 0))
    {
        return {};
    }

    std::string text(size, '\0');

    fn(g.instance, g.system, size, &size, text.data());

    std::vector<std::string> names;
    size_t start = 0;

    while (start < text.size()) {
        size_t end = text.find(' ', start);

        if (end == std::string::npos) {
            end = text.size();
        }

        std::string name = text.substr(start, end - start);

        name.erase(name.find_last_not_of('\0') + 1);

        if (!name.empty()) {
            names.push_back(name);
        }

        start = end + 1;
    }

    return names;
}

void destroy_all()
{

    for (EyeSwapchain& eye : g.eyes) {
        eye.images.clear();

        if (eye.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.handle);
        }

        eye = EyeSwapchain{};
    }

    if (g.space != XR_NULL_HANDLE) {
        xrDestroySpace(g.space);
    }

    if (g.session != XR_NULL_HANDLE) {
        xrDestroySession(g.session);
    }

    if (g.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g.instance);
    }

    g.space = XR_NULL_HANDLE;
    g.session = XR_NULL_HANDLE;
    g.instance = XR_NULL_HANDLE;
    g.running.store(0);
}

bool create_eye_swapchains(RT64::Application& app)
{

    XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t viewCount = 0;

    if (XR_FAILED(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views)) || (viewCount != 2))
    {
        return false;
    }

    uint32_t formatCount = 0;

    xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr);

    std::vector<int64_t> formats(formatCount);

    xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());

    // RT64's targets hold display-encoded RGBA8, so a raw copy into an sRGB image gives the runtime exactly what it decodes.
    bool srgb = false;

    for (int64_t f : formats) {
        srgb = srgb || (f == kVkFormatR8G8B8A8Srgb);
    }

    if (!srgb)
    {
        fprintf(stderr, "[vr] the runtime offers no R8G8B8A8 sRGB swapchain; running flat\n");
        return false;
    }

    for (uint32_t e = 0; e < 2; e++) {
        EyeSwapchain& eye = g.eyes[e];
        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };

        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = kVkFormatR8G8B8A8Srgb;
        info.sampleCount = 1;
        info.width = views[e].recommendedImageRectWidth;
        info.height = views[e].recommendedImageRectHeight;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;

        if (XR_FAILED(xrCreateSwapchain(g.session, &info, &eye.handle))) {
            return false;
        }

        eye.width = info.width;
        eye.height = info.height;

        uint32_t imageCount = 0;

        xrEnumerateSwapchainImages(eye.handle, 0, &imageCount, nullptr);

        std::vector<XrSwapchainImageVulkanKHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });

        xrEnumerateSwapchainImages(eye.handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

        for (const XrSwapchainImageVulkanKHR& image : images) {
            eye.images.push_back(plume::WrapVulkanImage(app.device.get(), image.image, plume::RenderFormat::R8G8B8A8_UNORM, eye.width, eye.height));
        }
    }

    fprintf(stderr, "[vr] session ready, eye swapchain %ux%u\n", g.eyes[0].width, g.eyes[0].height);

    return true;
}

// Session state changes drive xrBeginSession / xrEndSession; called once per headset frame.
void poll_events()
{

    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };

    while (xrPollEvent(g.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);

            if (changed->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };

                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g.running.store(XR_SUCCEEDED(xrBeginSession(g.session, &begin)) ? 1 : 0);
            }
            else if (changed->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g.session);
                g.running.store(0);
            }
            else if ((changed->state == XR_SESSION_STATE_EXITING) || (changed->state == XR_SESSION_STATE_LOSS_PENDING)) {
                g.running.store(0);
                fprintf(stderr, "[vr] session ended by the runtime; running flat\n");
            }
        }

        event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

XrPosef to_xr(const rs64vr::Pose& p)
{

    XrPosef pose;

    pose.orientation = { p.qx, p.qy, p.qz, p.qw };
    pose.position = { p.px, p.py, p.pz };

    return pose;
}
}

namespace rs64::vr {

bool requested()
{

    return env_on("ROGUESQ_VR") || rs64::video::vr_mode();
}

bool init_before_device()
{

    const char* extensions[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo info{ XR_TYPE_INSTANCE_CREATE_INFO };

    std::strncpy(info.applicationInfo.applicationName, kAppName, XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;

    const XrResult created = xrCreateInstance(&info, &g.instance);

    if (XR_FAILED(created))
    {
        fprintf(stderr, "[vr] no OpenXR runtime (xrCreateInstance %d); running flat\n", int(created));
        return false;
    }

    XrSystemGetInfo system{ XR_TYPE_SYSTEM_GET_INFO };

    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    const XrResult found = xrGetSystem(g.instance, &system, &g.system);

    if (XR_FAILED(found))
    {
        fprintf(stderr, "[vr] no headset (xrGetSystem %d); running flat\n", int(found));
        destroy_all();
        return false;
    }

    const bool loaded = load_function("xrGetVulkanInstanceExtensionsKHR", g.getInstanceExtensions)
        && load_function("xrGetVulkanDeviceExtensionsKHR", g.getDeviceExtensions)
        && load_function("xrGetVulkanGraphicsDeviceKHR", g.getGraphicsDevice)
        && load_function("xrGetVulkanGraphicsRequirementsKHR", g.getGraphicsRequirements);

    if (!loaded)
    {
        fprintf(stderr, "[vr] the runtime has no Vulkan support; running flat\n");
        destroy_all();
        return false;
    }

    // xrCreateSession fails unless the requirements were queried first.
    XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };

    g.getGraphicsRequirements(g.instance, g.system, &requirements);

    plume::VulkanCreateHooks hooks;

    hooks.instanceExtensions = extension_list(g.getInstanceExtensions);
    hooks.deviceExtensions = extension_list(g.getDeviceExtensions);
    hooks.pickPhysicalDevice = [](VkInstance vk) {

        VkPhysicalDevice device = VK_NULL_HANDLE;

        g.getGraphicsDevice(g.instance, g.system, vk, &device);

        return device;
    };

    plume::SetVulkanCreateHooks(hooks);

    fprintf(stderr, "[vr] OpenXR instance ready (%zu instance and %zu device extensions requested)\n", hooks.instanceExtensions.size(), hooks.deviceExtensions.size());

    return true;
}

bool start_session(RT64::Application& app)
{

    if (g.instance == XR_NULL_HANDLE)
    {
        return false;
    }

    const plume::VulkanNativeHandles native = plume::GetVulkanNativeHandles(app.device.get(), app.presentGraphicsWorker->commandQueue.get());
    XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };

    binding.instance = native.instance;
    binding.physicalDevice = native.physicalDevice;
    binding.device = native.device;
    binding.queueFamilyIndex = native.queueFamily;
    binding.queueIndex = native.queueIndex;

    XrSessionCreateInfo info{ XR_TYPE_SESSION_CREATE_INFO };

    info.next = &binding;
    info.systemId = g.system;

    const XrResult session = xrCreateSession(g.instance, &info, &g.session);

    if (XR_FAILED(session))
    {
        fprintf(stderr, "[vr] session creation failed (xrCreateSession %d); running flat\n", int(session));
        destroy_all();
        return false;
    }

    g.queueMutex = native.queueMutex;

    XrReferenceSpaceCreateInfo space{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };

    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;

    if (XR_FAILED(xrCreateReferenceSpace(g.session, &space, &g.space)) || !create_eye_swapchains(app))
    {
        fprintf(stderr, "[vr] session setup failed; running flat\n");
        destroy_all();
        return false;
    }

    return true;
}

void shutdown()
{

    destroy_all();
}
}

extern "C" int rs64_vr_session(void)
{

    return g.running.load();
}

extern "C" uint32_t rs64_vr_target_rate(void)
{

    const int64_t period = g.predictedPeriod.load();

    return (g.running.load() && (period > 0)) ? uint32_t(1e9 / double(period)) : 0;
}

// Game logic rate in VR, rounded down so the game never outruns the headset and fills RT64's queues; ROGUESQ_VR_HALF_RATE=1 halves it.
extern "C" int rs64_vr_game_hz(void)
{

    static const bool s_half = env_on("ROGUESQ_VR_HALF_RATE");
    const uint32_t rate = rs64_vr_target_rate();

    return int(s_half ? rate / 2 : rate);
}

extern "C" void rs64_vr_eye_size(uint32_t* width, uint32_t* height)
{

    *width = g.eyes[0].width;
    *height = g.eyes[0].height;
}

// ROGUESQ_VR_UNITS_PER_METER: game camera-space units per meter (default 1 until the world scale is measured).
extern "C" int rs64_vr_locate(uint32_t framesAhead, rs64vr::EyeViews* out)
{

    const int64_t time = g.predictedTime.load();

    out->valid = false;

    if (!g.running.load() || (time <= 0))
    {
        return 0;
    }

    XrViewLocateInfo info{ XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState state{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t count = 0;

    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time + int64_t(framesAhead) * g.predictedPeriod.load();
    info.space = g.space;

    if (XR_FAILED(xrLocateViews(g.session, &info, &state, 2, &count, views)) || (count != 2))
    {
        return 0;
    }

    for (int e = 0; e < 2; e++) {
        const XrPosef& p = views[e].pose;
        const XrFovf& f = views[e].fov;

        out->eye[e] = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z };
        out->fov[e] = { f.angleLeft, f.angleRight, f.angleUp, f.angleDown };
    }

    out->valid = (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

    return out->valid ? 1 : 0;
}

extern "C" int rs64_vr_begin_frame(void)
{

    if (g.session == XR_NULL_HANDLE)
    {
        return -1;
    }

    poll_events();

    if (!g.running.load())
    {
        return -1;
    }

    XrFrameState frame{ XR_TYPE_FRAME_STATE };

    if (XR_FAILED(xrWaitFrame(g.session, nullptr, &frame)))
    {
        return -1;
    }

    g.displayTime = frame.predictedDisplayTime;
    g.predictedTime.store(frame.predictedDisplayTime);
    g.predictedPeriod.store(frame.predictedDisplayPeriod);

    // The runtime may use the binding's VkQueue in begin, end, acquire and release (OpenXR 12.24.2).
    std::lock_guard<std::mutex> lock(*g.queueMutex);

    g.frameBegun = XR_SUCCEEDED(xrBeginFrame(g.session, nullptr));

    if (!g.frameBegun)
    {
        return -1;
    }

    return frame.shouldRender ? 1 : 0;
}

extern "C" plume::RenderTexture* rs64_vr_acquire_eye(int eye, uint32_t* width, uint32_t* height)
{

    EyeSwapchain& swapchain = g.eyes[eye];
    uint32_t index = 0;

    if (!g.frameBegun || (swapchain.handle == XR_NULL_HANDLE))
    {
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(*g.queueMutex);
    const bool acquired = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain.handle, nullptr, &index));

    lock.unlock();

    if (!acquired)
    {
        return nullptr;
    }

    XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(swapchain.handle, &wait);
    swapchain.acquired = index;
    *width = swapchain.width;
    *height = swapchain.height;

    return swapchain.images[index].get();
}

extern "C" void rs64_vr_end_frame(const rs64vr::EyeViews* views)
{

    if (!g.frameBegun)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(*g.queueMutex);

    for (EyeSwapchain& eye : g.eyes) {
        if (eye.acquired != kNotAcquired) {
            XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

            xrReleaseSwapchainImage(eye.handle, &release);
            eye.acquired = kNotAcquired;
            eye.released = true;
        }
    }

    // A layer shows each swapchain's last released image, so a repeated frame needs no new copy (OpenXR 10.4).
    XrCompositionLayerProjectionView projection[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer) };
    const bool show = (views != nullptr) && views->valid && g.eyes[0].released && g.eyes[1].released;

    for (int e = 0; show && (e < 2); e++) {
        projection[e].pose = to_xr(views->eye[e]);
        projection[e].fov = { views->fov[e].left, views->fov[e].right, views->fov[e].up, views->fov[e].down };
        projection[e].subImage.swapchain = g.eyes[e].handle;
        projection[e].subImage.imageRect = { { 0, 0 }, { int32_t(g.eyes[e].width), int32_t(g.eyes[e].height) } };
    }

    layer.space = g.space;
    layer.viewCount = 2;
    layer.views = projection;

    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };

    end.displayTime = g.displayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = show ? 1 : 0;
    end.layers = show ? layers : nullptr;
    xrEndFrame(g.session, &end);
    g.frameBegun = false;
}
