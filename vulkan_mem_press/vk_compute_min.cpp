#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <cstring>

#define VK_CHECK(x) do { \
    VkResult err = (x); \
    if (err != VK_SUCCESS) { \
        fprintf(stderr, "Vulkan error %d at %s:%d\n", (int)err, __FILE__, __LINE__); \
        std::exit(1); \
    } \
} while (0)

static std::vector<uint32_t> load_spv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        perror("open vk_compute_min.spv");
        std::exit(1);
    }
    std::streamsize sz = f.tellg();
    if (sz <= 0 || (sz % 4) != 0) {
        fprintf(stderr, "Invalid SPIR-V file size: %ld\n", (long)sz);
        std::exit(1);
    }
    f.seekg(0, std::ios::beg);
    std::vector<uint32_t> buf((size_t)sz / 4);
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        fprintf(stderr, "Failed to read SPIR-V file\n");
        std::exit(1);
    }
    return buf;
}

static const char* vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        default: return "VK_RESULT_UNKNOWN";
    }
}

int main() {
    // ---- Instance ----
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vk_compute_min";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %s (%d)\n", vk_result_str(r), (int)r);
        return 1;
    }

    // ---- Physical device ----
    uint32_t pd_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &pd_count, nullptr));
    if (pd_count == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &pd_count, pds.data()));
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("Using device: %s (apiVersion %u.%u.%u)\n",
           props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    // ---- Queue family (compute) ----
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    if (qf_count == 0) {
        fprintf(stderr, "No queue families found\n");
        return 1;
    }
    std::vector<VkQueueFamilyProperties> qfp(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfp.data());

    uint32_t compute_qf = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            compute_qf = i;
            break;
        }
    }
    if (compute_qf == UINT32_MAX) {
        fprintf(stderr, "No compute-capable queue family found\n");
        return 1;
    }

    // ---- Device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = compute_qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures feats{}; // keep default (all false) but non-null pointer

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;

    VkDevice dev = VK_NULL_HANDLE;
    r = vkCreateDevice(pd, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %s (%d)\n", vk_result_str(r), (int)r);
        vkDestroyInstance(inst, nullptr);
        return 1;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, compute_qf, 0, &queue);

    // ---- Load SPIR-V & create shader module ----
    auto spv = load_spv("vk_compute_min.spv");

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();

    VkShaderModule sm = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

    // ---- Pipeline layout (no descriptors) ----
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    // ---- Compute pipeline ----
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm;
    cpci.stage.pName  = "main";
    cpci.layout       = pl;

    VkPipeline pipe = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));

    // ---- Command pool & buffer ----
    VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_ci.queueFamilyIndex = compute_qf;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(dev, &pool_ci, nullptr, &pool));

    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cba, &cb));

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    // Dispatch a small workload (your shader是空的，只验证执行链路)
    vkCmdDispatch(cb, 1, 1, 1);

    VK_CHECK(vkEndCommandBuffer(cb));

    // ---- Submit & wait ----
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    printf("OK: Vulkan compute dispatch completed.\n");

    // ---- Cleanup ----
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyPipelineLayout(dev, pl, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
