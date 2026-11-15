#include "video/vr_openxr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SDL.h"
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
static constexpr int64_t kVkFormatB8G8R8A8Srgb = 50;
// The virtual screen for 2D screens: a 4:3 image on a quad straight ahead of where the session started.
static constexpr uint32_t kScreenWidth = 1280;
static constexpr uint32_t kScreenHeight = 960;
static constexpr float kScreenMeters = 1.8f;
static constexpr float kScreenDistance = 2.0f;
static constexpr uint32_t kNotAcquired = UINT32_MAX;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<std::unique_ptr<plume::RenderTexture>> images;
    uint32_t acquired = kNotAcquired;
    bool released = false;
};

struct PadAction {
    const char* name;
    const char* path;
};

// Indexed by rs64::vr::PadButton, then the stick.
static constexpr PadAction kPadActions[] = {
    { "a", "/user/hand/right/input/a/click" },
    { "b", "/user/hand/right/input/b/click" },
    { "x", "/user/hand/left/input/x/click" },
    { "menu", "/user/hand/left/input/menu/click" },
    { "stick-click-left", "/user/hand/left/input/thumbstick/click" },
    { "stick-click-right", "/user/hand/right/input/thumbstick/click" },
    { "trigger-left", "/user/hand/left/input/trigger/value" },
    { "trigger-right", "/user/hand/right/input/trigger/value" },
    { "stick", "/user/hand/left/input/thumbstick" },
};
static constexpr int kPadStick = rs64::vr::kPadButtons;
static_assert(std::size(kPadActions) == kPadStick + 1);

struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction pad[std::size(kPadActions)] = {};
    bool padReady = false;
    EyeSwapchain eyes[2];
    EyeSwapchain screen;
    std::mutex* queueMutex = nullptr;
    bool frameBegun = false;
    XrTime displayTime = 0;
    std::atomic<int> running{ 0 };
    bool sessionBegun = false;
    bool colorScale = false;
    // Brightness of the next submitted frame (fade between the virtual screen and the 3D view), 1 = full.
    float brightness = 1.0f;
    // Set by quit; while it waits, the calls other threads have in progress (a whole headset frame counts as one) drain to zero.
    std::atomic<int> quitting{ 0 };
    std::atomic<int> inUse{ 0 };
    std::atomic<int64_t> predictedTime{ 0 };
    std::atomic<int64_t> predictedPeriod{ 0 };
    PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR getGraphicsDevice = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
};

State g;

// Marks an OpenXR call from a thread other than the one that quits; false once quit has started, and the call must not go ahead.
struct XrUse {
    bool ok;

    XrUse()
    {

        g.inUse.fetch_add(1);
        ok = (g.quitting.load() == 0);
    }

    ~XrUse()
    {
        g.inUse.fetch_sub(1);
    }
};

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

    for (EyeSwapchain* eye : { &g.eyes[0], &g.eyes[1], &g.screen }) {
        eye->images.clear();

        if (eye->handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye->handle);
        }

        *eye = EyeSwapchain{};
    }

    if (g.space != XR_NULL_HANDLE) {
        xrDestroySpace(g.space);
    }

    if (g.actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(g.actionSet);
    }

    g.actionSet = XR_NULL_HANDLE;
    g.padReady = false;

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

    const auto create = [&](EyeSwapchain& out, int64_t format, plume::RenderFormat wrapFormat, uint32_t width, uint32_t height) {

        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };

        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;

        if (XR_FAILED(xrCreateSwapchain(g.session, &info, &out.handle))) {
            return false;
        }

        out.width = width;
        out.height = height;

        uint32_t imageCount = 0;

        xrEnumerateSwapchainImages(out.handle, 0, &imageCount, nullptr);

        std::vector<XrSwapchainImageVulkanKHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });

        xrEnumerateSwapchainImages(out.handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

        for (const XrSwapchainImageVulkanKHR& image : images) {
            out.images.push_back(plume::WrapVulkanImage(app.device.get(), image.image, wrapFormat, width, height));
        }

        return true;
    };

    for (uint32_t e = 0; e < 2; e++) {
        if (!create(g.eyes[e], kVkFormatR8G8B8A8Srgb, plume::RenderFormat::R8G8B8A8_UNORM, views[e].recommendedImageRectWidth, views[e].recommendedImageRectHeight)) {
            return false;
        }
    }

    // The window's VI pass draws BGRA8, so the virtual screen is BGRA8 too and takes the same raw copy as the eyes.
    bool bgraSrgb = false;

    for (int64_t f : formats) {
        bgraSrgb = bgraSrgb || (f == kVkFormatB8G8R8A8Srgb);
    }

    if (!bgraSrgb || !create(g.screen, kVkFormatB8G8R8A8Srgb, plume::RenderFormat::B8G8R8A8_UNORM, kScreenWidth, kScreenHeight)) {
        g.screen = EyeSwapchain{};
        fprintf(stderr, "[vr] no B8G8R8A8 sRGB swapchain; menus stay in the 3D view\n");
    }

    fprintf(stderr, "[vr] session ready, eye swapchain %ux%u\n", g.eyes[0].width, g.eyes[0].height);

    return true;
}

// Touch controller actions, suggested for the Oculus Touch profile; other controllers get the runtime's remap of it.
bool create_pad_actions()
{

    XrActionSetCreateInfo set{ XR_TYPE_ACTION_SET_CREATE_INFO };

    std::strncpy(set.actionSetName, "flight", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set.localizedActionSetName, "Flight", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);

    if (XR_FAILED(xrCreateActionSet(g.instance, &set, &g.actionSet)))
    {
        return false;
    }

    std::vector<XrActionSuggestedBinding> suggested;

    for (size_t i = 0; i < std::size(kPadActions); i++) {
        XrActionCreateInfo action{ XR_TYPE_ACTION_CREATE_INFO };
        XrPath path = XR_NULL_PATH;

        action.actionType = (int(i) == kPadStick) ? XR_ACTION_TYPE_VECTOR2F_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT;
        std::strncpy(action.actionName, kPadActions[i].name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(action.localizedActionName, kPadActions[i].name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

        if (XR_FAILED(xrCreateAction(g.actionSet, &action, &g.pad[i])) || XR_FAILED(xrStringToPath(g.instance, kPadActions[i].path, &path)))
        {
            return false;
        }

        suggested.push_back({ g.pad[i], path });
    }

    XrInteractionProfileSuggestedBinding profile{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

    profile.countSuggestedBindings = uint32_t(suggested.size());
    profile.suggestedBindings = suggested.data();

    if (XR_FAILED(xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller", &profile.interactionProfile)) || XR_FAILED(xrSuggestInteractionProfileBindings(g.instance, &profile)))
    {
        return false;
    }

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };

    attach.countActionSets = 1;
    attach.actionSets = &g.actionSet;

    return XR_SUCCEEDED(xrAttachSessionActionSets(g.session, &attach));
}

// Session state changes drive xrBeginSession / xrEndSession; called once per headset frame, and until the session runs from rs64_vr_session.
void poll_events()
{

    static std::mutex s_poll;
    std::unique_lock<std::mutex> lock(s_poll, std::try_to_lock);

    if (!lock.owns_lock())
    {
        return;
    }

    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };

    while (xrPollEvent(g.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);

            if (changed->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };

                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult began = xrBeginSession(g.session, &begin);

                g.sessionBegun = XR_SUCCEEDED(began);
                g.running.store(g.sessionBegun ? 1 : 0);
                fprintf(stderr, "[vr] xrBeginSession %d\n", int(began));
            }
            else if (changed->state == XR_SESSION_STATE_STOPPING) {
                g.running.store(0);
                xrEndSession(g.session);
                g.sessionBegun = false;
            }
            else if ((changed->state == XR_SESSION_STATE_EXITING) && (g.quitting.load() == 0)) {
                SDL_Event quit{};

                g.running.store(0);
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
                fprintf(stderr, "[vr] quit from the headset\n");
            }
            else if (changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                g.running.store(0);
                fprintf(stderr, "[vr] session lost; running flat\n");
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

    return rs64::video::vr_mode();
}

bool init_before_device()
{

    // Color scale dims the layers for the fade between the virtual screen and the 3D view; without it the fade's dark half is black frames.
    uint32_t available = 0;

    xrEnumerateInstanceExtensionProperties(nullptr, 0, &available, nullptr);

    std::vector<XrExtensionProperties> properties(available, { XR_TYPE_EXTENSION_PROPERTIES });

    xrEnumerateInstanceExtensionProperties(nullptr, available, &available, properties.data());

    for (const XrExtensionProperties& p : properties) {
        g.colorScale = g.colorScale || (std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME) == 0);
    }

    fprintf(stderr, "[vr] view changes fade %s\n", g.colorScale ? "through black" : "with black frames (no color scale)");

    const char* extensions[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME };
    XrInstanceCreateInfo info{ XR_TYPE_INSTANCE_CREATE_INFO };

    std::strncpy(info.applicationInfo.applicationName, kAppName, XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = g.colorScale ? 2 : 1;
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

    g.padReady = create_pad_actions();

    if (!g.padReady) {
        fprintf(stderr, "[vr] controller actions failed; no VR controller input\n");
    }

    return true;
}

void quit()
{

    if (g.instance == XR_NULL_HANDLE)
    {
        return;
    }

    // Destroying the session under a frame another thread is still in, without ending it first, left a process the driver never let exit.
    g.quitting.store(1);

    const Uint32 start = SDL_GetTicks();

    while ((g.inUse.load() != 0) && (SDL_GetTicks() - start < 1000)) {
        SDL_Delay(1);
    }

    if (g.inUse.load() != 0) {
        fprintf(stderr, "[vr] quit: a headset call is still in progress\n");
    }

    if (g.sessionBegun) {
        xrRequestExitSession(g.session);

        const Uint32 requested = SDL_GetTicks();

        while (g.sessionBegun && (SDL_GetTicks() - requested < 1000)) {
            poll_events();
            SDL_Delay(1);
        }

        if (g.sessionBegun) {
            xrEndSession(g.session);
            g.sessionBegun = false;
        }
    }

    g.running.store(0);

    if (g.queueMutex) {
        std::lock_guard<std::mutex> lock(*g.queueMutex);

        destroy_all();
    } else {
        destroy_all();
    }

    fprintf(stderr, "[vr] session closed\n");

#if defined(_WIN32)

    // _Exit left a windowless process holding the headset; ending the process here skips the VR runtime's own DLL shutdown.
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);

#endif
}

bool poll_pad(Pad* out)
{

    *out = Pad{};

    const XrUse use;

    if (!use.ok || !g.padReady || !g.running.load())
    {
        return false;
    }

    const XrActiveActionSet active{ g.actionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };

    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;

    // XR_SESSION_NOT_FOCUSED is a success code: the headset menu or another app has the input.
    if (xrSyncActions(g.session, &sync) != XR_SUCCESS)
    {
        return false;
    }

    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };

    for (int i = 0; i < kPadStick; i++) {
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };

        info.action = g.pad[i];

        if (XR_SUCCEEDED(xrGetActionStateBoolean(g.session, &info, &state)) && state.isActive && state.currentState) {
            out->held |= 1u << i;
        }
    }

    XrActionStateVector2f stick{ XR_TYPE_ACTION_STATE_VECTOR2F };

    info.action = g.pad[kPadStick];

    if (XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &info, &stick)) && stick.isActive) {
        out->stick_x = stick.currentState.x;
        out->stick_y = stick.currentState.y;
    }

    return true;
}
}

extern "C" int rs64_vr_session(void)
{

    if (g.quitting.load() != 0)
    {
        return 0;
    }

    // Frames only start once the session runs, so the READY event that starts it is polled here.
    if (!g.running.load() && (g.session != XR_NULL_HANDLE)) {
        poll_events();
    }

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

extern "C" int rs64_vr_locate(uint32_t framesAhead, rs64vr::EyeViews* out)
{

    const int64_t time = g.predictedTime.load();
    const XrUse use;

    out->valid = false;

    if (!use.ok || !g.running.load() || (time <= 0))
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

    static bool s_logged = false;

    if (out->valid && !s_logged) {
        s_logged = true;

        for (int e = 0; e < 2; e++) {
            const rs64vr::Pose& p = out->eye[e];
            const rs64vr::Fov& f = out->fov[e];

            fprintf(stderr, "[vr] eye %d pos %.4f %.4f %.4f rot %.4f %.4f %.4f %.4f fov L %.4f R %.4f U %.4f D %.4f\n", e, p.px, p.py, p.pz, p.qx, p.qy, p.qz, p.qw, f.left, f.right, f.up, f.down);
        }
    }

    return out->valid ? 1 : 0;
}

extern "C" int rs64_vr_begin_frame(void)
{

    // The frame counts as in use from here until rs64_vr_end_frame, or until this returns -1.
    g.inUse.fetch_add(1);

    const auto fail = []() {

        g.inUse.fetch_sub(1);
        return -1;
    };

    if ((g.quitting.load() != 0) || (g.session == XR_NULL_HANDLE))
    {
        return fail();
    }

    poll_events();

    if (!g.running.load())
    {
        return fail();
    }

    XrFrameState frame{ XR_TYPE_FRAME_STATE };

    if (XR_FAILED(xrWaitFrame(g.session, nullptr, &frame)))
    {
        return fail();
    }

    g.displayTime = frame.predictedDisplayTime;
    g.predictedTime.store(frame.predictedDisplayTime);
    g.predictedPeriod.store(frame.predictedDisplayPeriod);

    // The runtime may use the binding's VkQueue in begin, end, acquire and release (OpenXR 12.24.2).
    std::lock_guard<std::mutex> lock(*g.queueMutex);

    g.frameBegun = XR_SUCCEEDED(xrBeginFrame(g.session, nullptr));

    if (!g.frameBegun)
    {
        return fail();
    }

    return frame.shouldRender ? 1 : 0;
}

static plume::RenderTexture* acquire(EyeSwapchain& swapchain, uint32_t* width, uint32_t* height)
{

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

extern "C" plume::RenderTexture* rs64_vr_acquire_eye(int eye, uint32_t* width, uint32_t* height)
{
    return acquire(g.eyes[eye], width, height);
}

extern "C" plume::RenderTexture* rs64_vr_acquire_screen(uint32_t* width, uint32_t* height)
{
    return acquire(g.screen, width, height);
}

extern "C" int rs64_vr_has_screen(void)
{
    return (g.screen.handle != XR_NULL_HANDLE) ? 1 : 0;
}

extern "C" void rs64_vr_set_brightness(float brightness)
{
    g.brightness = brightness;
}

// Caller holds the queue mutex.
static void release_acquired()
{

    for (EyeSwapchain* eye : { &g.eyes[0], &g.eyes[1], &g.screen }) {
        if (eye->acquired != kNotAcquired) {
            XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

            xrReleaseSwapchainImage(eye->handle, &release);
            eye->acquired = kNotAcquired;
            eye->released = true;
        }
    }
}

// Caller holds the queue mutex. A brightness under 1 dims the layer, or below half drops it (black) when the runtime has no color scale.
static void end_with(XrCompositionLayerBaseHeader* layer)
{

    XrCompositionLayerColorScaleBiasKHR scale{ XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR };
    const float b = std::clamp(g.brightness, 0.0f, 1.0f);

    if ((layer != nullptr) && (b < 1.0f)) {
        if (g.colorScale) {
            scale.colorScale = { b, b, b, 1.0f };
            scale.colorBias = { 0.0f, 0.0f, 0.0f, 0.0f };
            layer->next = &scale;
        }
        else if (b < 0.5f) {
            layer = nullptr;
        }
    }

    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };

    end.displayTime = g.displayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = (layer != nullptr) ? 1 : 0;
    end.layers = (layer != nullptr) ? &layer : nullptr;
    xrEndFrame(g.session, &end);
    g.frameBegun = false;
    g.inUse.fetch_sub(1);
}

extern "C" void rs64_vr_end_frame_screen(void)
{

    if (!g.frameBegun)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(*g.queueMutex);

    release_acquired();

    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const bool show = g.screen.released;

    quad.space = g.space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.screen.handle;
    quad.subImage.imageRect = { { 0, 0 }, { int32_t(g.screen.width), int32_t(g.screen.height) } };
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = { 0.0f, 0.0f, -kScreenDistance };
    quad.size = { kScreenMeters, kScreenMeters * float(kScreenHeight) / float(kScreenWidth) };

    end_with(show ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad) : nullptr);
}

extern "C" void rs64_vr_end_frame(const rs64vr::EyeViews* views)
{

    if (!g.frameBegun)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(*g.queueMutex);

    release_acquired();

    // A layer shows each swapchain's last released image, so a repeated frame needs no new copy (OpenXR 10.4).
    XrCompositionLayerProjectionView projection[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
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

    end_with(show ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) : nullptr);
}
