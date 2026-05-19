#include <vulkan/vulkan.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t g_stop_requested = 0;
static volatile std::sig_atomic_t g_last_signal = 0;

#define VK_CHECK(x)                                                                      \
    do                                                                                   \
    {                                                                                    \
        VkResult err = (x);                                                              \
        if (err != VK_SUCCESS)                                                           \
        {                                                                                \
            fprintf(stderr, "Vulkan error %d at %s:%d\n", (int)err, __FILE__, __LINE__); \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

static const char *vkres(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    default:
        return "VK_RESULT_OTHER";
    }
}

static std::vector<uint32_t> load_spv(const char *path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        perror("open spv");
        std::exit(1);
    }
    auto sz = f.tellg();
    if (sz <= 0 || (sz % 4) != 0)
    {
        fprintf(stderr, "Invalid SPIR-V size: %lld\n", (long long)sz);
        std::exit(1);
    }
    f.seekg(0, std::ios::beg);
    std::vector<uint32_t> buf((size_t)sz / 4);
    f.read(reinterpret_cast<char *>(buf.data()), sz);
    return buf;
}

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    {
        if ((typeBits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & want) == want))
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static void fill_pattern_u32(uint32_t *dst, size_t words)
{
    for (size_t i = 0; i < words; ++i)
    {
        dst[i] = (uint32_t)i * 2654435761u;
    }
}

static void handle_termination_signal(int signum)
{
    g_stop_requested = 1;
    g_last_signal = signum;
}

struct Args
{
    uint32_t mb = 512;           // data buffer size in MB
    std::string mode = "rdwr";   // rd / wr / rdwr
    uint32_t stride_bytes = 64;  // stride in bytes (multiple of 4)
    uint32_t iters = 200;        // "logical" iters target (we split into chunks)
    uint32_t elems_per_inv = 64; // words per invocation per iter
    uint32_t workgroups = 4096;  // number of workgroups
    uint32_t seconds = 10;       // duration
    uint32_t chunk_iters = 40;   // max iters per dispatch (watchdog-safe knob)
    uint32_t dispatches_per_submit = 1;
    uint32_t warmup_submits = 0;
    bool warmup_submits_explicit = false;
    bool benchmark = false;
    bool gpu_timing = false;
    bool strict_device_local = false;
    std::string buffer_layout = "output";
    std::string spv_path = "./vulkan_mem_press/memstress_alu.spv"; // path to SPIR-V file
};

static Args parse_args(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; i++)
    {
        std::string k = argv[i];
        auto need = [&](const char *name) -> const char *
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (k == "--mb")
            a.mb = (uint32_t)std::stoul(need("--mb"));
        else if (k == "--mode")
            a.mode = need("--mode");
        else if (k == "--stride")
            a.stride_bytes = (uint32_t)std::stoul(need("--stride"));
        else if (k == "--iters")
            a.iters = (uint32_t)std::stoul(need("--iters"));
        else if (k == "--einv")
            a.elems_per_inv = (uint32_t)std::stoul(need("--einv"));
        else if (k == "--wg")
            a.workgroups = (uint32_t)std::stoul(need("--wg"));
        else if (k == "--seconds")
            a.seconds = (uint32_t)std::stoul(need("--seconds"));
        else if (k == "--chunk-iters")
            a.chunk_iters = (uint32_t)std::stoul(need("--chunk-iters"));
        else if (k == "--dispatches-per-submit")
            a.dispatches_per_submit = (uint32_t)std::stoul(need("--dispatches-per-submit"));
        else if (k == "--warmup-submits")
        {
            a.warmup_submits = (uint32_t)std::stoul(need("--warmup-submits"));
            a.warmup_submits_explicit = true;
        }
        else if (k == "--benchmark")
            a.benchmark = true;
        else if (k == "--gpu-timing")
            a.gpu_timing = true;
        else if (k == "--strict-device-local")
            a.strict_device_local = true;
        else if (k == "--buffer-layout")
            a.buffer_layout = need("--buffer-layout");
        else if (k == "--spv")
            a.spv_path = need("--spv");
        else if (k == "--help")
        {
            std::printf(
                "Usage: ./vk_memstress_safe [options]\n"
                "  --mb N            data buffer size in MB (default 512)\n"
                "  --mode rd|wr|rdwr  (default rdwr)\n"
                "  --stride N         stride in bytes, multiple of 4 (default 64)\n"
                "  --iters N          logical iters target (default 200)\n"
                "  --chunk-iters N    max iters per dispatch (default 40, watchdog-safe knob)\n"
                "  --dispatches-per-submit N  dispatches grouped into one submit (default 1)\n"
                "  --warmup-submits N warmup submits before timing (default 0)\n"
                "  --einv N           words per invocation per iter (default 64)\n"
                "  --wg N             workgroups (default 4096)\n"
                "  --seconds N        duration seconds (default 10)\n"
                "  --benchmark        benchmark mode: GPU-timed batched submits\n"
                "  --gpu-timing       collect GPU timestamp timing if supported\n"
                "  --strict-device-local  require DEVICE_LOCAL buffers\n"
                "  --buffer-layout output|copy  binding1 is per-invocation output or copy destination (default output)\n"
                "  --spv PATH         path to SPIR-V file (default ./vulkan_mem_press/memstress_alu.spv)\n");
            std::exit(0);
        }
        else
        {
            fprintf(stderr, "Unknown arg: %s (use --help)\n", k.c_str());
            std::exit(1);
        }
    }
    if (a.stride_bytes < 4)
        a.stride_bytes = 4;
    if ((a.stride_bytes % 4) != 0)
    {
        fprintf(stderr, "--stride must be multiple of 4\n");
        std::exit(1);
    }
    if (a.elems_per_inv == 0)
        a.elems_per_inv = 1;
    if (a.workgroups == 0)
        a.workgroups = 1;
    if (a.iters == 0)
        a.iters = 1;
    if (a.seconds == 0)
        a.seconds = 1;
    if (a.chunk_iters == 0)
        a.chunk_iters = 1;
    if (a.dispatches_per_submit == 0)
        a.dispatches_per_submit = 1;
    if (a.buffer_layout != "output" && a.buffer_layout != "copy")
    {
        fprintf(stderr, "--buffer-layout must be output|copy\n");
        std::exit(1);
    }

    if (a.benchmark)
    {
        a.gpu_timing = true;
        a.strict_device_local = true;
        if (a.dispatches_per_submit == 1)
            a.dispatches_per_submit = 16;
        if (!a.warmup_submits_explicit)
            a.warmup_submits = 2;
    }

    return a;
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::signal(SIGTERM, handle_termination_signal);
    std::signal(SIGINT, handle_termination_signal);

    Args args = parse_args(argc, argv);
    const uint32_t local_size_x = 256;
    const uint64_t invocations = (uint64_t)args.workgroups * local_size_x;

    uint32_t mode = 2;
    if (args.mode == "rd")
        mode = 0;
    else if (args.mode == "wr")
        mode = 1;
    else if (args.mode == "rdwr")
        mode = 2;
    else
    {
        fprintf(stderr, "--mode must be rd|wr|rdwr\n");
        return 1;
    }

    // ---- Instance ----
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vk_memstress_safe";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    // ---- Physical device ----
    uint32_t pd_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &pd_count, nullptr));
    if (pd_count == 0)
    {
        fprintf(stderr, "No Vulkan physical devices\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &pd_count, pds.data()));
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("Using device: %s\n", props.deviceName);

    // ---- Queue family ----
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfp.data());

    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++)
    {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            qf = i;
            break;
        }
    }
    if (qf == UINT32_MAX)
    {
        fprintf(stderr, "No compute queue family\n");
        return 1;
    }

    const bool queue_has_timestamps = (qfp[qf].timestampValidBits != 0) && (props.limits.timestampPeriod > 0.0f);
    if (args.gpu_timing && !queue_has_timestamps)
    {
        fprintf(stderr, "GPU timestamps are not supported on this compute queue\n");
        return 1;
    }

    // ---- Device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;

    VkDevice dev = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // ---- Buffers ----
    const VkDeviceSize data_bytes = (VkDeviceSize)args.mb * 1024ull * 1024ull;
    const bool copy_layout = (args.buffer_layout == "copy");
    const VkDeviceSize out_bytes = copy_layout ? data_bytes : invocations * sizeof(uint32_t);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer dataBuf = VK_NULL_HANDLE;
    bci.size = data_bytes;
    VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &dataBuf));

    VkBuffer outBuf = VK_NULL_HANDLE;
    bci.size = out_bytes;
    VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &outBuf));

    VkMemoryRequirements mrData{}, mrOut{};
    vkGetBufferMemoryRequirements(dev, dataBuf, &mrData);
    vkGetBufferMemoryRequirements(dev, outBuf, &mrOut);

    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mtData = find_mem_type(pd, mrData.memoryTypeBits, want);
    uint32_t mtOut = find_mem_type(pd, mrOut.memoryTypeBits, want);

    if (mtData == UINT32_MAX || mtOut == UINT32_MAX)
    {
        if (args.strict_device_local)
        {
            fprintf(stderr, "DEVICE_LOCAL buffers are required in the selected mode\n");
            return 1;
        }
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        mtData = find_mem_type(pd, mrData.memoryTypeBits, want);
        mtOut = find_mem_type(pd, mrOut.memoryTypeBits, want);
        if (mtData == UINT32_MAX || mtOut == UINT32_MAX)
        {
            fprintf(stderr, "No suitable memory type found\n");
            return 1;
        }
        std::printf("Memory: using HOST_VISIBLE|COHERENT\n");
    }
    else
    {
        std::printf("Memory: using DEVICE_LOCAL\n");
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    VkDeviceMemory memData = VK_NULL_HANDLE;
    mai.allocationSize = mrData.size;
    mai.memoryTypeIndex = mtData;
    VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &memData));
    VK_CHECK(vkBindBufferMemory(dev, dataBuf, memData, 0));

    VkDeviceMemory memOut = VK_NULL_HANDLE;
    mai.allocationSize = mrOut.size;
    mai.memoryTypeIndex = mtOut;
    VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &memOut));
    VK_CHECK(vkBindBufferMemory(dev, outBuf, memOut, 0));

    // ---- Descriptor set ----
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding b1{};
    b1.binding = 1;
    b1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b1.descriptorCount = 1;
    b1.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding bindings[2] = {b0, b1};

    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;

    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &dsl));

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 24; // 6*u32

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;

    VkPipelineLayout pl = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 2;

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));

    VkDescriptorBufferInfo dbi0{dataBuf, 0, data_bytes};
    VkDescriptorBufferInfo dbi1{outBuf, 0, out_bytes};

    VkWriteDescriptorSet wds[2]{};
    wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[0].dstSet = ds;
    wds[0].dstBinding = 0;
    wds[0].descriptorCount = 1;
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[0].pBufferInfo = &dbi0;

    wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[1].dstSet = ds;
    wds[1].dstBinding = 1;
    wds[1].descriptorCount = 1;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[1].pBufferInfo = &dbi1;

    vkUpdateDescriptorSets(dev, 2, wds, 0, nullptr);

    // ---- Shader & pipeline ----
    auto spv = load_spv(args.spv_path.c_str());

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();

    VkShaderModule sm = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm;
    cpci.stage.pName = "main";
    cpci.layout = pl;

    VkPipeline pipe = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));

    // ---- Command pool / buffer ----
    VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.queueFamilyIndex = qf;
    cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(dev, &cpci2, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));

    auto submit_init_commands = [&](auto &&record_commands)
    {
        VK_CHECK(vkResetFences(dev, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cb, 0));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        record_commands(cb);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    };

    auto add_transfer_barrier = [&](VkCommandBuffer init_cb)
    {
        VkBufferMemoryBarrier barriers[2]{};

        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = dataBuf;
        barriers[0].offset = 0;
        barriers[0].size = VK_WHOLE_SIZE;

        barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].buffer = outBuf;
        barriers[1].offset = 0;
        barriers[1].size = VK_WHOLE_SIZE;

        vkCmdPipelineBarrier(
            init_cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            2,
            barriers,
            0,
            nullptr);
    };

    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        void *p = nullptr;
        VK_CHECK(vkMapMemory(dev, memData, 0, data_bytes, 0, &p));
        fill_pattern_u32((uint32_t *)p, (size_t)(data_bytes / 4));
        vkUnmapMemory(dev, memData);

        VK_CHECK(vkMapMemory(dev, memOut, 0, out_bytes, 0, &p));
        std::memset(p, 0, (size_t)out_bytes);
        vkUnmapMemory(dev, memOut);
    }
    else
    {
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;

        VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbci.size = data_bytes;
        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        sbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(dev, &sbci, nullptr, &stagingBuf));

        VkMemoryRequirements stagingReq{};
        vkGetBufferMemoryRequirements(dev, stagingBuf, &stagingReq);
        uint32_t stagingType = find_mem_type(
            pd,
            stagingReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (stagingType == UINT32_MAX)
        {
            fprintf(stderr, "No HOST_VISIBLE|COHERENT staging memory type found\n");
            return 1;
        }

        VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        stagingAlloc.allocationSize = stagingReq.size;
        stagingAlloc.memoryTypeIndex = stagingType;
        VK_CHECK(vkAllocateMemory(dev, &stagingAlloc, nullptr, &stagingMem));
        VK_CHECK(vkBindBufferMemory(dev, stagingBuf, stagingMem, 0));

        void *p = nullptr;
        VK_CHECK(vkMapMemory(dev, stagingMem, 0, data_bytes, 0, &p));
        fill_pattern_u32((uint32_t *)p, (size_t)(data_bytes / 4));
        vkUnmapMemory(dev, stagingMem);

        submit_init_commands([&](VkCommandBuffer init_cb)
                             {
            VkBufferCopy copy{0, 0, data_bytes};
            vkCmdCopyBuffer(init_cb, stagingBuf, dataBuf, 1, &copy);
            vkCmdFillBuffer(init_cb, outBuf, 0, out_bytes, 0);
            add_transfer_barrier(init_cb); });

        vkFreeMemory(dev, stagingMem, nullptr);
        vkDestroyBuffer(dev, stagingBuf, nullptr);
    }

    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (args.gpu_timing)
    {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 2;
        VK_CHECK(vkCreateQueryPool(dev, &qpci, nullptr, &queryPool));
    }

    // ---- Push constants ----
    struct PC
    {
        uint32_t total_words, word_stride, iters, elems_per_inv, mode, seed;
    } pc{};
    pc.total_words = (uint32_t)(data_bytes / 4);
    pc.word_stride = (uint32_t)(args.stride_bytes / 4);
    pc.elems_per_inv = args.elems_per_inv;
    pc.mode = mode;
    pc.seed = 0x12345678u;

    auto bytes_for_one_dispatch = [&](uint32_t iters_for_dispatch) -> uint64_t
    {
        uint64_t words_per_iter = invocations * (uint64_t)pc.elems_per_inv;
        uint64_t bytes_per_iter = words_per_iter * 4ull * ((mode == 2) ? 2ull : 1ull);
        return bytes_per_iter * (uint64_t)iters_for_dispatch;
    };

    std::printf("Config:\n");
    std::printf("  data: %u MB, mode=%s, stride=%uB, iters=%u (logical), chunk-iters=%u, einv=%u, wg=%u\n",
                args.mb, args.mode.c_str(), args.stride_bytes, args.iters, args.chunk_iters, args.elems_per_inv, args.workgroups);

    std::printf("  exec: %s, dispatches/submit=%u, warmup-submits=%u\n",
                args.benchmark ? "benchmark" : "stress",
                args.dispatches_per_submit,
                args.warmup_submits);
    std::printf("  buffer-layout: %s\n", args.buffer_layout.c_str());

    std::printf("  est bytes/chunk-dispatch (chunk-iters=%u): %.3f GB\n",
                args.chunk_iters, (double)bytes_for_one_dispatch(args.chunk_iters) / (1024.0 * 1024.0 * 1024.0));

    std::printf("Running %u seconds...\n", args.seconds);

    auto t0 = std::chrono::steady_clock::now();
    auto t_end = t0 + std::chrono::seconds(args.seconds);

    uint64_t dispatches_completed = 0;
    uint64_t measured_submits_completed = 0;
    uint64_t total_est_bytes = 0;
    uint64_t total_gpu_ns = 0;
    uint32_t current_chunk_iters = args.chunk_iters;
    auto warmup_start = t0;
    auto warmup_end = t0;
    auto measured_start = t0;

    auto submit_batch = [&](const std::vector<uint32_t> &dispatch_iters, bool measure) -> VkResult
    {
        if (dispatch_iters.empty())
            return VK_SUCCESS;

        VK_CHECK(vkResetFences(dev, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cb, 0));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        if (measure && args.gpu_timing)
        {
            vkCmdResetQueryPool(cb, queryPool, 0, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        }

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);

        PC submit_pc = pc;
        for (uint32_t iters_for_dispatch : dispatch_iters)
        {
            submit_pc.iters = iters_for_dispatch;
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &submit_pc);
            vkCmdDispatch(cb, args.workgroups, 1, 1);
            submit_pc.seed += 0x9e3779b9u;
        }

        if (measure && args.gpu_timing)
        {
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        }

        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;

        VkResult r1 = vkQueueSubmit(queue, 1, &si, fence);
        if (r1 != VK_SUCCESS)
            return r1;

        VkResult r2 = VK_TIMEOUT;
        while (r2 == VK_TIMEOUT)
        {
            r2 = vkWaitForFences(dev, 1, &fence, VK_TRUE, 100000000ull);
            if (r2 == VK_TIMEOUT && g_stop_requested)
                return VK_TIMEOUT;
        }

        if (r2 == VK_SUCCESS && measure && args.gpu_timing)
        {
            uint64_t timestamps[2] = {};
            VkResult qr = vkGetQueryPoolResults(
                dev,
                queryPool,
                0,
                2,
                sizeof(timestamps),
                timestamps,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (qr != VK_SUCCESS)
                return qr;
            total_gpu_ns += (uint64_t)((double)(timestamps[1] - timestamps[0]) * props.limits.timestampPeriod);
        }
        return r2;
    };

    auto build_dispatch_batch = [&](uint32_t remaining) -> std::vector<uint32_t>
    {
        std::vector<uint32_t> dispatch_iters;
        dispatch_iters.reserve(args.dispatches_per_submit);
        while (remaining > 0 && dispatch_iters.size() < args.dispatches_per_submit)
        {
            uint32_t it = (remaining > current_chunk_iters) ? current_chunk_iters : remaining;
            dispatch_iters.push_back(it);
            remaining -= it;
        }
        return dispatch_iters;
    };

    auto bytes_for_batch = [&](const std::vector<uint32_t> &dispatch_iters) -> uint64_t
    {
        uint64_t total = 0;
        for (uint32_t iters_for_dispatch : dispatch_iters)
        {
            total += bytes_for_one_dispatch(iters_for_dispatch);
        }
        return total;
    };

    if (args.warmup_submits > 0)
    {
        std::vector<uint32_t> warmup_dispatches(args.dispatches_per_submit, current_chunk_iters);
        for (uint32_t warmup = 0; warmup < args.warmup_submits; ++warmup)
        {
            if (g_stop_requested)
                goto done;
            VkResult rr = submit_batch(warmup_dispatches, false);
            if (rr == VK_TIMEOUT && g_stop_requested)
                goto done;
            if (rr != VK_SUCCESS)
            {
                std::fprintf(stderr, "Warmup submit failed: %s (%d)\n", vkres(rr), (int)rr);
                goto done;
            }
            pc.seed += 0x9e3779b9u * (uint32_t)warmup_dispatches.size();
        }
        warmup_end = std::chrono::steady_clock::now();
    }
    else
    {
        warmup_end = t0;
    }

    measured_start = warmup_end;

    // Watchdog-safe loop:
    // - Each "logical iters" is split into multiple chunk dispatches.
    // - If DEVICE_LOST occurs, we automatically reduce chunk-iters and retry (best-effort).
    while (std::chrono::steady_clock::now() < t_end)
    {
        if (g_stop_requested)
            break;
        uint32_t remaining = args.iters;
        while (remaining > 0 && std::chrono::steady_clock::now() < t_end)
        {
            if (g_stop_requested)
                goto done;
            std::vector<uint32_t> dispatch_iters = build_dispatch_batch(remaining);

            VkResult rr = submit_batch(dispatch_iters, true);
            if (rr == VK_TIMEOUT && g_stop_requested)
                goto done;
            if (rr == VK_ERROR_DEVICE_LOST)
            {
                if (current_chunk_iters > 1)
                {
                    current_chunk_iters = (current_chunk_iters + 1) / 2;
                    std::fprintf(stderr,
                                 "WARN: DEVICE_LOST. Reducing chunk-iters to %u and retrying...\n",
                                 current_chunk_iters);
                    // small pause to avoid immediate re-trigger
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue; // retry with smaller chunk
                }
                else
                {
                    std::fprintf(stderr, "FATAL: DEVICE_LOST even at chunk-iters=1.\n");
                    goto done;
                }
            }
            else if (rr != VK_SUCCESS)
            {
                std::fprintf(stderr, "Vulkan submit/wait failed: %s (%d)\n", vkres(rr), (int)rr);
                goto done;
            }

            uint32_t consumed_iters = 0;
            for (uint32_t iters_for_dispatch : dispatch_iters)
                consumed_iters += iters_for_dispatch;
            pc.seed += 0x9e3779b9u * (uint32_t)dispatch_iters.size();
            remaining -= consumed_iters;
            dispatches_completed += dispatch_iters.size();
            measured_submits_completed += 1;
            total_est_bytes += bytes_for_batch(dispatch_iters);
        }
    }

done:
    auto t1 = std::chrono::steady_clock::now();
    double total_wall_sec = std::chrono::duration<double>(t1 - t0).count();
    double warmup_wall_sec = std::chrono::duration<double>(warmup_end - warmup_start).count();
    double measured_wall_sec = std::chrono::duration<double>(t1 - measured_start).count();
    double gb = (double)total_est_bytes / (1024.0 * 1024.0 * 1024.0);
    double gbps = (total_wall_sec > 0.0) ? (gb / total_wall_sec) : 0.0;
    double measured_gpu_sec = (double)total_gpu_ns / 1e9;
    double gpu_gbps = (measured_gpu_sec > 0.0) ? (gb / measured_gpu_sec) : 0.0;

    std::printf("Done.\n");
    std::printf("  dispatches_completed: %llu\n", (unsigned long long)dispatches_completed);
    std::printf("  measured_submits_completed: %llu\n", (unsigned long long)measured_submits_completed);
    std::printf("  time: %.3f s\n", total_wall_sec);
    std::printf("  warmup wall time: %.3f s\n", warmup_wall_sec);
    std::printf("  measured wall time: %.3f s\n", measured_wall_sec);
    std::printf("  est traffic: %.3f GB\n", gb);
    std::printf("  est throughput: %.3f GB/s\n", gbps);
    if (args.gpu_timing)
    {
        std::printf("  measured gpu time: %.6f s\n", measured_gpu_sec);
        std::printf("  gpu throughput: %.3f GB/s\n", gpu_gbps);
    }
    std::printf("  final chunk-iters used: %u\n", current_chunk_iters);
    if (g_stop_requested)
    {
        std::printf("  terminated by signal: %d\n", (int)g_last_signal);
        std::fflush(stdout);
    }

    vkDeviceWaitIdle(dev);

    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, cmdPool, 1, &cb);
    vkDestroyCommandPool(dev, cmdPool, nullptr);

    if (queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(dev, queryPool, nullptr);
    }

    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);

    vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyPipelineLayout(dev, pl, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);

    vkFreeMemory(dev, memOut, nullptr);
    vkFreeMemory(dev, memData, nullptr);
    vkDestroyBuffer(dev, outBuf, nullptr);
    vkDestroyBuffer(dev, dataBuf, nullptr);

    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
