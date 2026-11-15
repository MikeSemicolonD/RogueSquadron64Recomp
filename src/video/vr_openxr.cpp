#include "video/vr_openxr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
    { "grip-left", "/user/hand/left/input/squeeze/value" },
    { "grip-right", "/user/hand/right/input/squeeze/value" },
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
    XrAction grip = XR_NULL_HANDLE;
    // Right thumbstick, which the game doesn't use: up and down set the player's size (vrScale).
    XrAction scaleStick = XR_NULL_HANDLE;
    Uint32 scaleTicks = 0;
    XrSpace handSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    EyeSwapchain eyes[2];
    EyeSwapchain screen;
    // The controller layer, drawn every headset frame over the eyes or the virtual screen; handsDrawn = both images acquired this frame.
    EyeSwapchain hands[2];
    bool handsDrawn = false;
    rs64vr::EyeViews handViews;
    // The runtime's controller models (XR_FB_render_model) as triangles of position and normal; handModelReady 1 once loaded, -1 when one can't be read.
    bool renderModels = false;
    PFN_xrEnumerateRenderModelPathsFB enumerateModelPaths = nullptr;
    PFN_xrGetRenderModelPropertiesFB getModelProperties = nullptr;
    PFN_xrLoadRenderModelFB loadModel = nullptr;
    std::vector<float> handModels[2];
    std::atomic<int> handModelReady[2] = { 0, 0 };
    Uint32 nextModelTry = 0;
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

    for (EyeSwapchain* eye : { &g.eyes[0], &g.eyes[1], &g.screen, &g.hands[0], &g.hands[1] }) {
        eye->images.clear();

        if (eye->handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye->handle);
        }

        *eye = EyeSwapchain{};
    }

    for (XrSpace& hand : g.handSpaces) {
        if (hand != XR_NULL_HANDLE) {
            xrDestroySpace(hand);
        }

        hand = XR_NULL_HANDLE;
    }

    if (g.space != XR_NULL_HANDLE) {
        xrDestroySpace(g.space);
    }

    if (g.actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(g.actionSet);
    }

    g.actionSet = XR_NULL_HANDLE;
    g.grip = XR_NULL_HANDLE;
    g.scaleStick = XR_NULL_HANDLE;
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

    // A render target is drawn through a view in wrapFormat, so its sRGB image must allow that format.
    const auto create = [&](EyeSwapchain& out, int64_t format, plume::RenderFormat wrapFormat, uint32_t width, uint32_t height, bool renderTarget = false) {

        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };

        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | (renderTarget ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0);
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
            out.images.push_back(plume::WrapVulkanImage(app.device.get(), image.image, wrapFormat, width, height, renderTarget));
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

    for (uint32_t e = 0; e < 2; e++) {
        if (!create(g.hands[e], kVkFormatR8G8B8A8Srgb, plume::RenderFormat::R8G8B8A8_UNORM, g.eyes[e].width, g.eyes[e].height, true)) {
            g.hands[0].images.clear();

            if (g.hands[0].handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(g.hands[0].handle);
            }

            g.hands[0] = EyeSwapchain{};
            g.hands[1] = EyeSwapchain{};
            fprintf(stderr, "[vr] no controller layer swapchain; controllers are not drawn\n");
            break;
        }
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

    // Both controllers' grip poses, to draw them.
    XrPath handPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };
    XrPath gripPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };
    XrActionCreateInfo grip{ XR_TYPE_ACTION_CREATE_INFO };

    xrStringToPath(g.instance, "/user/hand/left", &handPaths[0]);
    xrStringToPath(g.instance, "/user/hand/right", &handPaths[1]);
    xrStringToPath(g.instance, "/user/hand/left/input/grip/pose", &gripPaths[0]);
    xrStringToPath(g.instance, "/user/hand/right/input/grip/pose", &gripPaths[1]);
    grip.actionType = XR_ACTION_TYPE_POSE_INPUT;
    grip.countSubactionPaths = 2;
    grip.subactionPaths = handPaths;
    std::strncpy(grip.actionName, "grip", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(grip.localizedActionName, "grip", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

    if (XR_SUCCEEDED(xrCreateAction(g.actionSet, &grip, &g.grip))) {
        suggested.push_back({ g.grip, gripPaths[0] });
        suggested.push_back({ g.grip, gripPaths[1] });
    }

    XrActionCreateInfo scale{ XR_TYPE_ACTION_CREATE_INFO };
    XrPath scalePath = XR_NULL_PATH;

    scale.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(scale.actionName, "player-size", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(scale.localizedActionName, "player size", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

    if (XR_SUCCEEDED(xrCreateAction(g.actionSet, &scale, &g.scaleStick)) && XR_SUCCEEDED(xrStringToPath(g.instance, "/user/hand/right/input/thumbstick", &scalePath))) {
        suggested.push_back({ g.scaleStick, scalePath });
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

    if (XR_FAILED(xrAttachSessionActionSets(g.session, &attach)))
    {
        return false;
    }

    for (int h = 0; (h < 2) && (g.grip != XR_NULL_HANDLE); h++) {
        XrActionSpaceCreateInfo space{ XR_TYPE_ACTION_SPACE_CREATE_INFO };

        space.action = g.grip;
        space.subactionPath = handPaths[h];
        space.poseInActionSpace.orientation.w = 1.0f;

        if (XR_FAILED(xrCreateActionSpace(g.session, &space, &g.handSpaces[h]))) {
            g.handSpaces[h] = XR_NULL_HANDLE;
        }
    }

    return true;
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
        g.renderModels = g.renderModels || (std::strcmp(p.extensionName, XR_FB_RENDER_MODEL_EXTENSION_NAME) == 0);
    }

    fprintf(stderr, "[vr] view changes fade %s, controllers are %s\n", g.colorScale ? "through black" : "with black frames (no color scale)",
            g.renderModels ? "the runtime's models" : "stand-ins (no render models)");

    std::vector<const char*> extensions = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };

    if (g.colorScale) {
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    }

    if (g.renderModels) {
        extensions.push_back(XR_FB_RENDER_MODEL_EXTENSION_NAME);
    }

    XrInstanceCreateInfo info{ XR_TYPE_INSTANCE_CREATE_INFO };

    std::strncpy(info.applicationInfo.applicationName, kAppName, XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.enabledExtensionNames = extensions.data();

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

    g.renderModels = g.renderModels && load_function("xrEnumerateRenderModelPathsFB", g.enumerateModelPaths)
        && load_function("xrGetRenderModelPropertiesFB", g.getModelProperties) && load_function("xrLoadRenderModelFB", g.loadModel);

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

    // Full tilt grows or shrinks the player by about 1.4x a second; the video config saves the new size once it settles.
    XrActionStateVector2f scale{ XR_TYPE_ACTION_STATE_VECTOR2F };
    const Uint32 now = SDL_GetTicks();
    const float dt = std::min(float(now - g.scaleTicks) / 1000.0f, 0.1f);

    g.scaleTicks = now;
    info.action = g.scaleStick;

    if ((g.scaleStick != XR_NULL_HANDLE) && XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &info, &scale)) && scale.isActive && (std::fabs(scale.currentState.y) > 0.3f)) {
        rs64::video::set_vr_scale(rs64::video::vr_scale() * std::exp2(0.5f * scale.currentState.y * dt));

        static Uint32 s_logged = 0;

        if (now - s_logged > 500) {
            s_logged = now;
            fprintf(stderr, "[vr] player size %.2f\n", rs64::video::vr_scale());
        }
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

// Game logic rate in VR: half the headset rate, each logic frame rendered twice at fresh head poses. At the full rate the present thread fell out of
// step with the headset and about a third of finished frames never reached it (53 of 90 a second); ROGUESQ_VR_FULL_RATE=1 runs the full rate.
extern "C" int rs64_vr_game_hz(void)
{

    static const bool s_full = env_on("ROGUESQ_VR_FULL_RATE");
    const uint32_t rate = rs64_vr_target_rate();

    return int(s_full ? rate : rate / 2);
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

// Flattens a binary glTF into triangles of position and normal (6 floats a vertex) in the model's space, through its node transforms. Reads float
// positions and normals and triangle-list primitives; false when the file can't be read that way.
static bool glb_triangles(const std::vector<uint8_t>& glb, std::vector<float>& out)
{

    using nlohmann::json;

    const auto u32 = [&](size_t at) {

        uint32_t v = 0;

        std::memcpy(&v, glb.data() + at, sizeof(v));

        return v;
    };

    constexpr uint32_t kMagic = 0x46546C67u;
    constexpr uint32_t kJsonChunk = 0x4E4F534Au;
    constexpr uint32_t kBinChunk = 0x004E4942u;

    if ((glb.size() < 28) || (u32(0) != kMagic) || (u32(16) != kJsonChunk))
    {
        return false;
    }

    const size_t jsonLength = u32(12);
    const size_t binAt = 20 + jsonLength;

    if ((binAt + 8 > glb.size()) || (u32(binAt + 4) != kBinChunk) || (binAt + 8 + u32(binAt) > glb.size()))
    {
        return false;
    }

    const uint8_t* bin = glb.data() + binAt + 8;
    const size_t binLength = u32(binAt);
    const json j = json::parse(glb.begin() + 20, glb.begin() + 20 + jsonLength, nullptr, false);

    if (j.is_discarded())
    {
        return false;
    }

    try {
        // Element i of an accessor: its address in the binary chunk, or null past it.
        const auto element = [&](const json& accessor, uint32_t i, size_t size) -> const uint8_t* {

            const json& view = j.at("bufferViews").at(accessor.at("bufferView").get<size_t>());
            const size_t stride = view.value("byteStride", size_t(0));
            const size_t at = view.value("byteOffset", size_t(0)) + accessor.value("byteOffset", size_t(0)) + i * ((stride > 0) ? stride : size);

            return (at + size <= binLength) ? (bin + at) : nullptr;
        };

        const auto vec3 = [&](const json& accessor, uint32_t i, const float m[16], float w, float* outXyz) {

            const uint8_t* p = element(accessor, i, sizeof(float) * 3);
            float v[3] = {};

            if (p != nullptr) {
                std::memcpy(v, p, sizeof(v));
            }

            for (int r = 0; r < 3; r++) {
                outXyz[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * w;
            }
        };

        const auto index = [&](const json& accessor, uint32_t i) -> uint32_t {

            const int type = accessor.at("componentType").get<int>();
            const size_t size = (type == 5125) ? 4 : ((type == 5123) ? 2 : 1);
            const uint8_t* p = element(accessor, i, size);
            uint32_t v = 0;

            if (p != nullptr) {
                std::memcpy(&v, p, size);
            }

            return v;
        };

        const auto draw_mesh = [&](const json& mesh, const float m[16]) {

            for (const json& prim : mesh.at("primitives")) {
                const json& attributes = prim.at("attributes");

                if ((prim.value("mode", 4) != 4) || !attributes.contains("POSITION")) {
                    continue;
                }

                const json& positions = j.at("accessors").at(attributes.at("POSITION").get<size_t>());

                if (positions.at("componentType").get<int>() != 5126) {
                    continue;
                }

                const json* normals = attributes.contains("NORMAL") ? &j.at("accessors").at(attributes.at("NORMAL").get<size_t>()) : nullptr;
                const json* indices = prim.contains("indices") ? &j.at("accessors").at(prim.at("indices").get<size_t>()) : nullptr;
                const uint32_t count = (indices != nullptr) ? indices->at("count").get<uint32_t>() : positions.at("count").get<uint32_t>();

                for (uint32_t t = 0; t + 2 < count; t += 3) {
                    float p[3][3];
                    float n[3][3];

                    for (int k = 0; k < 3; k++) {
                        const uint32_t v = (indices != nullptr) ? index(*indices, t + k) : (t + k);

                        vec3(positions, v, m, 1.0f, p[k]);

                        if (normals != nullptr) {
                            vec3(*normals, v, m, 0.0f, n[k]);
                        }
                    }

                    // Without normals the face's own normal lights it.
                    if (normals == nullptr) {
                        const float a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
                        const float b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
                        const float face[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };

                        for (int k = 0; k < 3; k++) {
                            std::memcpy(n[k], face, sizeof(face));
                        }
                    }

                    for (int k = 0; k < 3; k++) {
                        const float length = std::sqrt(n[k][0] * n[k][0] + n[k][1] * n[k][1] + n[k][2] * n[k][2]);
                        const float scale = (length > 0.0f) ? (1.0f / length) : 0.0f;

                        out.insert(out.end(), { p[k][0], p[k][1], p[k][2], n[k][0] * scale, n[k][1] * scale, n[k][2] * scale });
                    }
                }
            }
        };

        // A node's world transform is its parent's times its own (a column-major matrix, or translation * rotation * scale).
        std::function<void(size_t, const float*)> visit = [&](size_t nodeIndex, const float* parent) {

            const json& node = j.at("nodes").at(nodeIndex);
            float local[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

            if (node.contains("matrix")) {
                for (int i = 0; i < 16; i++) {
                    local[i] = node.at("matrix").at(i).get<float>();
                }
            }
            else {
                const json t = node.value("translation", json::array({ 0.0f, 0.0f, 0.0f }));
                const json q = node.value("rotation", json::array({ 0.0f, 0.0f, 0.0f, 1.0f }));
                const json s = node.value("scale", json::array({ 1.0f, 1.0f, 1.0f }));
                const float x = q.at(0).get<float>(), y = q.at(1).get<float>(), z = q.at(2).get<float>(), w = q.at(3).get<float>();
                const float r[9] = {
                    1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                    2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                    2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)
                };

                for (int c = 0; c < 3; c++) {
                    for (int row = 0; row < 3; row++) {
                        local[c * 4 + row] = r[c * 3 + row] * s.at(c).get<float>();
                    }

                    local[12 + c] = t.at(c).get<float>();
                }
            }

            float world[16];

            for (int c = 0; c < 4; c++) {
                for (int row = 0; row < 4; row++) {
                    world[c * 4 + row] = parent[row] * local[c * 4] + parent[4 + row] * local[c * 4 + 1] + parent[8 + row] * local[c * 4 + 2] + parent[12 + row] * local[c * 4 + 3];
                }
            }

            if (node.contains("mesh")) {
                draw_mesh(j.at("meshes").at(node.at("mesh").get<size_t>()), world);
            }

            for (const json& child : node.value("children", json::array())) {
                visit(child.get<size_t>(), world);
            }
        };

        static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const json& scene = j.at("scenes").at(j.value("scene", size_t(0)));

        for (const json& root : scene.at("nodes")) {
            visit(root.get<size_t>(), kIdentity);
        }
    }
    catch (const json::exception&) {
        return false;
    }

    return !out.empty();
}

// A runtime's model saved next to the game, loaded on runtimes that offer none (Meta Link does not; the XR Simulator does). The player's own copy, never shipped.
static constexpr const char* kSavedModels[2] = { "roguesq_controller_left.glb", "roguesq_controller_right.glb" };

static void use_hand_model(int h, const std::vector<uint8_t>& glb, const char* from)
{

    std::vector<float> triangles;
    const bool read = glb_triangles(glb, triangles);

    g.handModels[h] = std::move(triangles);
    g.handModelReady[h] = read ? 1 : -1;

    fprintf(stderr, "[vr] controller model from %s: %s, %zu triangles\n", from, read ? "loaded" : "unreadable, stand-in drawn", g.handModels[h].size() / 18);
}

// Present thread, at most once a second until both are in: the runtime offers a controller's model once that controller is tracked.
static void load_hand_models()
{

    constexpr const char* kModelPaths[2] = { "/model_fb/controller/left", "/model_fb/controller/right" };
    const Uint32 now = SDL_GetTicks();

    if (((g.handModelReady[0] != 0) && (g.handModelReady[1] != 0)) || (now < g.nextModelTry))
    {
        return;
    }

    if (!g.renderModels) {
        for (int h = 0; h < 2; h++) {
            FILE* f = std::fopen(kSavedModels[h], "rb");
            std::vector<uint8_t> glb;

            if (f != nullptr) {
                std::fseek(f, 0, SEEK_END);
                glb.resize(size_t(std::max(std::ftell(f), 0L)));
                std::fseek(f, 0, SEEK_SET);
                glb.resize(std::fread(glb.data(), 1, glb.size(), f));
                std::fclose(f);
            }

            if (glb.empty()) {
                g.handModelReady[h] = -1;
            }
            else {
                use_hand_model(h, glb, kSavedModels[h]);
            }
        }

        return;
    }

    g.nextModelTry = now + 1000;

    // A path must have been listed by the runtime before its properties can be asked for.
    uint32_t pathCount = 0;

    g.enumerateModelPaths(g.session, 0, &pathCount, nullptr);

    std::vector<XrRenderModelPathInfoFB> paths(pathCount, { XR_TYPE_RENDER_MODEL_PATH_INFO_FB });

    g.enumerateModelPaths(g.session, pathCount, &pathCount, paths.data());

    for (int h = 0; h < 2; h++) {
        XrPath path = XR_NULL_PATH;
        XrRenderModelCapabilitiesRequestFB request{ XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB };
        XrRenderModelPropertiesFB properties{ XR_TYPE_RENDER_MODEL_PROPERTIES_FB };

        request.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB;
        properties.next = &request;

        const bool offered = (g.handModelReady[h] == 0) && XR_SUCCEEDED(xrStringToPath(g.instance, kModelPaths[h], &path))
            && (g.getModelProperties(g.session, path, &properties) == XR_SUCCESS) && (properties.modelKey != XR_NULL_RENDER_MODEL_KEY_FB);

        if (!offered) {
            continue;
        }

        XrRenderModelLoadInfoFB load{ XR_TYPE_RENDER_MODEL_LOAD_INFO_FB };
        XrRenderModelBufferFB buffer{ XR_TYPE_RENDER_MODEL_BUFFER_FB };
        std::vector<uint8_t> glb;

        load.modelKey = properties.modelKey;

        if (XR_SUCCEEDED(g.loadModel(g.session, &load, &buffer)) && (buffer.bufferCountOutput > 0)) {
            glb.resize(buffer.bufferCountOutput);
            buffer.bufferCapacityInput = uint32_t(glb.size());
            buffer.buffer = glb.data();
        }

        if (glb.empty() || XR_FAILED(g.loadModel(g.session, &load, &buffer))) {
            g.handModelReady[h] = -1;
            continue;
        }

        use_hand_model(h, glb, properties.modelName);

        // Kept for runtimes without models; a copy the player already has stays.
        FILE* saved = (g.handModelReady[h] == 1) ? std::fopen(kSavedModels[h], "rb") : nullptr;

        if (saved != nullptr) {
            std::fclose(saved);
        }
        else if ((g.handModelReady[h] == 1) && ((saved = std::fopen(kSavedModels[h], "wb")) != nullptr)) {
            std::fwrite(glb.data(), 1, glb.size(), saved);
            std::fclose(saved);
            fprintf(stderr, "[vr] saved %s\n", kSavedModels[h]);
        }
    }
}

extern "C" int rs64_vr_hand_model(int hand, const float** vertices, uint32_t* count)
{

    if (g.handModelReady[hand] != 1)
    {
        return 0;
    }

    *vertices = g.handModels[hand].data();
    *count = uint32_t(g.handModels[hand].size() / 6);

    return 1;
}

extern "C" int rs64_vr_hands_frame(rs64vr::EyeViews* views, rs64vr::Pose hands[2], int valid[2])
{

    load_hand_models();

    valid[0] = 0;
    valid[1] = 0;

    if (!g.frameBegun || (g.hands[0].handle == XR_NULL_HANDLE) || !rs64_vr_locate(0, views))
    {
        return 0;
    }

    constexpr XrSpaceLocationFlags kTracked = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;

    for (int h = 0; h < 2; h++) {
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };

        if ((g.handSpaces[h] == XR_NULL_HANDLE) || XR_FAILED(xrLocateSpace(g.handSpaces[h], g.space, g.displayTime, &location)) || ((location.locationFlags & kTracked) != kTracked)) {
            continue;
        }

        const XrPosef& p = location.pose;

        hands[h] = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z };
        valid[h] = 1;
    }

    g.handViews = *views;

    return (valid[0] || valid[1]) ? 1 : 0;
}

extern "C" plume::RenderTexture* rs64_vr_acquire_hands(int eye, uint32_t* width, uint32_t* height)
{

    plume::RenderTexture* image = acquire(g.hands[eye], width, height);

    g.handsDrawn = (eye == 1) && (image != nullptr) && (g.hands[0].acquired != kNotAcquired);

    return image;
}

// Caller holds the queue mutex.
static void release_acquired()
{

    for (EyeSwapchain* eye : { &g.eyes[0], &g.eyes[1], &g.screen, &g.hands[0], &g.hands[1] }) {
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

    // The controllers go on top in a layer of their own, at full brightness, blended by their alpha.
    XrCompositionLayerProjectionView handViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection hands{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t count = 0;

    if (layer != nullptr) {
        layers[count++] = layer;
    }

    if (g.handsDrawn) {
        for (int e = 0; e < 2; e++) {
            handViews[e].pose = to_xr(g.handViews.eye[e]);
            handViews[e].fov = { g.handViews.fov[e].left, g.handViews.fov[e].right, g.handViews.fov[e].up, g.handViews.fov[e].down };
            handViews[e].subImage.swapchain = g.hands[e].handle;
            handViews[e].subImage.imageRect = { { 0, 0 }, { int32_t(g.hands[e].width), int32_t(g.hands[e].height) } };
        }

        hands.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        hands.space = g.space;
        hands.viewCount = 2;
        hands.views = handViews;
        layers[count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&hands);
    }

    g.handsDrawn = false;

    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };

    end.displayTime = g.displayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = count;
    end.layers = (count > 0) ? layers : nullptr;
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
