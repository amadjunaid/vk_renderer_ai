#include "vk/VulkanContext.hpp"

#include "core/Window.hpp"  // pulls in GLFW (and therefore the Vulkan headers)
#include "vk/VkCheck.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*userData*/) {
    const char* level = "info";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        level = "error";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        level = "warning";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        level = "verbose";
    }

    std::cerr << "[vulkan " << level << "] " << data->pMessage << '\n';
    return VK_FALSE;
}

const char* deviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other";
    }
}

/// True when the loader advertises the given instance extension.
///
/// Local to this file: whether an extension exists is a property of the loader,
/// not of the renderer's state.
bool hasInstanceExtension(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }

    std::vector<VkExtensionProperties> available(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) != VK_SUCCESS) {
        return false;
    }

    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& ext) {
                           return std::string(ext.extensionName) == name;
                       });
}

/// True when the physical device advertises the given device extension.
bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& ext) {
                           return std::string(ext.extensionName) == name;
                       });
}

/// Name of the VK_KHR_portability_subset extension.
///
/// Spelled out instead of using VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME: that
/// macro lives in vulkan_beta.h, which vulkan.h only includes when
/// VK_ENABLE_BETA_EXTENSIONS is defined. Only the name string is needed to
/// enable the extension, so a local constant keeps every provisional extension
/// out of the rest of the build.
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";

/// Extensions the logical device must enable.
///
/// VK_KHR_swapchain is needed to present. VK_KHR_portability_subset is
/// advertised by portability implementations (MoltenVK on macOS, and portability
/// drivers elsewhere) and the spec requires it to be enabled whenever it is
/// present - vkCreateDevice fails otherwise.
std::vector<const char*> requiredDeviceExtensions(VkPhysicalDevice device) {
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    if (hasDeviceExtension(device, kPortabilitySubsetExtension)) {
        extensions.push_back(kPortabilitySubsetExtension);
    }

    return extensions;
}

}  // namespace

VulkanContext::VulkanContext(const Window& window, bool enableValidation) {
    validationEnabled_ = enableValidation && checkValidationLayerSupport();
    if (enableValidation && !validationEnabled_) {
        std::cerr << "[vulkan] " << kValidationLayer
                  << " is not available (install the Vulkan SDK); continuing without validation\n";
    }

    createInstance();
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice();
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (debugMessenger_ != VK_NULL_HANDLE) {
        const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn != nullptr) {
            destroyFn(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

bool VulkanContext::checkValidationLayerSupport() {
    std::uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS || layerCount == 0) {
        return false;
    }

    std::vector<VkLayerProperties> layers(layerCount);
    if (vkEnumerateInstanceLayerProperties(&layerCount, layers.data()) != VK_SUCCESS) {
        return false;
    }

    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::string(layer.layerName) == kValidationLayer;
    });
}

std::vector<const char*> VulkanContext::requiredInstanceExtensions(bool portabilityEnumeration) const {
    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        throw std::runtime_error("glfwGetRequiredInstanceExtensions returned no extensions");
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (portabilityEnumeration) {
        // Without this the macOS loader hides MoltenVK and
        // vkEnumeratePhysicalDevices reports zero devices. On platforms that do
        // not need it the extension is simply absent and this is skipped.
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    // VK_EXT_debug_utils is supplied by the validation layer rather than the
    // loader, so check for it instead of assuming it is there.
    if (validationEnabled_ && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

void VulkanContext::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Triangle";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "deepseek_vk_renderer";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);

    // Ask for 1.2 (more than enough for a triangle) but stay compatible with
    // older loaders that only report 1.0/1.1.
    std::uint32_t supportedVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&supportedVersion) != VK_SUCCESS) {
        supportedVersion = VK_API_VERSION_1_0;
    }
    appInfo.apiVersion = std::min(supportedVersion, static_cast<std::uint32_t>(VK_API_VERSION_1_2));
    apiVersion_ = appInfo.apiVersion;

    const bool portabilityEnumeration =
        hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    const std::vector<const char*> extensions = requiredInstanceExtensions(portabilityEnumeration);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (portabilityEnumeration) {
        // The flag only affects enumeration, so it stays paired with the
        // extension above: setting one without the other is a validation error.
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (validationEnabled_) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &kValidationLayer;
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));
}

void VulkanContext::setupDebugMessenger() {
    if (!validationEnabled_) {
        return;
    }

    const auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (createFn == nullptr) {
        std::cerr << "[vulkan] vkCreateDebugUtilsMessengerEXT is unavailable; debug output disabled\n";
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &debugCallback;

    VK_CHECK(createFn(instance_, &createInfo, nullptr, &debugMessenger_));
}

void VulkanContext::createSurface(const Window& window) {
    surface_ = window.createSurface(instance_);
}

QueueFamilyIndices VulkanContext::queryQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport == VK_TRUE) {
            indices.present = i;
        }

        if (indices.isComplete()) {
            break;
        }
    }

    return indices;
}

SwapchainSupport VulkanContext::querySwapchainSupport(VkPhysicalDevice device) const {
    SwapchainSupport details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount > 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    std::uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                                  details.presentModes.data());
    }

    return details;
}

int VulkanContext::scorePhysicalDevice(VkPhysicalDevice device) const {
    // -1 means "cannot be used at all".
    if (!queryQueueFamilies(device).isComplete()) {
        return -1;
    }
    if (!hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        return -1;
    }
    if (!querySwapchainSupport(device).isAdequate()) {
        return -1;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);

    switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1000;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 300;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 100;
        default: return 10;
    }
}

void VulkanContext::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr));
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan capable GPU was found on this system");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()));

    int bestScore = -1;
    for (VkPhysicalDevice device : devices) {
        const int score = scorePhysicalDevice(device);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = device;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE || bestScore < 0) {
        throw std::runtime_error(
            "No GPU offers a graphics queue, a present queue and " VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    std::cout << "[vulkan] using " << properties.deviceName << " ("
              << deviceTypeName(properties.deviceType) << ")\n";
}

void VulkanContext::createLogicalDevice() {
    const QueueFamilyIndices indices = queryQueueFamilies(physicalDevice_);
    graphicsFamily_ = indices.graphics.value();
    presentFamily_ = indices.present.value();

    const float queuePriority = 1.0f;
    const std::set<std::uint32_t> uniqueFamilies{graphicsFamily_, presentFamily_};

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());
    for (const std::uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    // A single hard-coded triangle needs no optional device features.
    VkPhysicalDeviceFeatures features{};

    const std::vector<const char*> deviceExtensions = requiredDeviceExtensions(physicalDevice_);

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    // Device level validation layers are deprecated and ignored by modern
    // loaders; the instance level layer already covers the whole device.

    VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
}
