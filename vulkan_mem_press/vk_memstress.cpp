#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#define VK_CHECK(x) do { \
    VkResult err = (x); \
    if (err != VK_SUCCESS) { \
        fprintf(stderr, "Vulkan error %d at %s:%d\n", (int)err, __FILE__, __LINE__); \
        std::exit(1); \
    } \
} while (0)

static const char* vkres(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        default: return "VK_RESULT_OTHER";
    }
}

static std::vector<uint32_t> load_spv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { perror("open spv"); std::exit(1); }
    auto sz = f.tellg();
    if (sz <= 0 || (sz % 4) != 0) {
        fprintf(stderr, "Invalid SPIR-V size: %lld\n", (long long)sz);
        std::exit(1);
    }
    f.seekg(0, std::ios::beg);
    std::vector<uint32_t> buf((size_t)sz / 4);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & want) == want)) {
            return i;
        }
    }
    return UINT32_MAX;
}

struct Args {
    uint32_t mb = 512;                 // data buffer size in MB
    std::string mode = "rdwr";         // rd / wr / rdwr
    uint32_t stride_bytes = 64;        // stride in bytes (multiple of 4)
    uint32_t iters = 200;              // "logical" iters target (we split into chunks)
    uint32_t elems_per_inv = 64;       // words per invocation per iter
    uint32_t workgroups = 4096;        // number of workgroups
    uint32_t seconds = 10;             // duration
    uint32_t chunk_iters = 40;         // max iters per dispatch (watchdog-safe knob)
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto need = [&](const char* name)->const char*{
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", name); std::exit(1); }
            return argv[++i];
        };

        if (k == "--mb") a.mb = (uint32_t)std::stoul(need("--mb"));
        else if (k == "--mode") a.mode = need("--mode");
        else if (k == "--stride") a.stride_bytes = (uint32_t)std::stoul(need("--stride"));
        else if (k == "--iters") a.iters = (uint32_t)std::stoul(need("--iters"));
        else if (k == "--einv") a.elems_per_inv = (uint32_t)std::stoul(need("--einv"));
        else if (k == "--wg") a.workgroups = (uint32_t)std::stoul(need("--wg"));
        else if (k == "--seconds") a.seconds = (uint32_t)std::stoul(need("--seconds"));
        else if (k == "--chunk-iters") a.chunk_iters = (uint32_t)std::stoul(need("--chunk-iters"));
        else if (k == "--help") {
            std::printf(
                "Usage: ./vk_memstress_safe [options]\n"
                "  --mb N            data buffer size in MB (default 512)\n"
                "  --mode rd|wr|rdwr  (default rdwr)\n"
                "  --stride N         stride in bytes, multiple of 4 (default 64)\n"
                "  --iters N          logical iters target (default 200)\n"
                "  --chunk-iters N    max iters per dispatch (default 40, watchdog-safe knob)\n"
                "  --einv N           words per invocation per iter (default 64)\n"
                "  --wg N             workgroups (default 4096)\n"
                "  --seconds N        duration seconds (default 10)\n"
            );
            std::exit(0);
        } else {
            fprintf(stderr, "Unknown arg: %s (use --help)\n", k.c_str());
            std::exit(1);
        }
    }
    if (a.stride_bytes < 4) a.stride_bytes = 4;
    if ((a.stride_bytes % 4) != 0) { fprintf(stderr, "--stride must be multiple of 4\n"); std::exit(1); }
    if (a.elems_per_inv == 0) a.elems_per_inv = 1;
    if (a.workgroups == 0) a.workgroups = 1;
    if (a.iters == 0) a.iters = 1;
    if (a.seconds == 0) a.seconds = 1;
    if (a.chunk_iters == 0) a.chunk_iters = 1;
    return a;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    uint32_t mode = 2;
    if (args.mode == "rd") mode = 0;
    else if (args.mode == "wr") mode = 1;
    else if (args.mode == "rdwr") mode = 2;
    else { fprintf(stderr, "--mode must be rd|wr|rdwr\n"); return 1; }

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
    if (pd_count == 0) { fprintf(stderr, "No Vulkan physical devices\n"); return 1; }
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
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    }
    if (qf == UINT32_MAX) { fprintf(stderr, "No compute queue family\n"); return 1; }

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
    const VkDeviceSize out_bytes  = 4;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
    uint32_t mtOut  = find_mem_type(pd, mrOut.memoryTypeBits, want);

    if (mtData == UINT32_MAX || mtOut == UINT32_MAX) {
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        mtData = find_mem_type(pd, mrData.memoryTypeBits, want);
        mtOut  = find_mem_type(pd, mrOut.memoryTypeBits, want);
        if (mtData == UINT32_MAX || mtOut == UINT32_MAX) {
            fprintf(stderr, "No suitable memory type found\n");
            return 1;
        }
        std::printf("Memory: using HOST_VISIBLE|COHERENT\n");
    } else {
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

    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(dev, memData, 0, data_bytes, 0, &p));
        uint32_t* w = (uint32_t*)p;
        size_t words = (size_t)(data_bytes / 4);
        for (size_t i = 0; i < words; i++) w[i] = (uint32_t)i * 2654435761u;
        vkUnmapMemory(dev, memData);

        VK_CHECK(vkMapMemory(dev, memOut, 0, out_bytes, 0, &p));
        std::memset(p, 0, (size_t)out_bytes);
        vkUnmapMemory(dev, memOut);
    }

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
    VkDescriptorBufferInfo dbi1{outBuf,  0, out_bytes};

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
    auto spv = load_spv("memstress.spv");

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();

    VkShaderModule sm = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm;
    cpci.stage.pName  = "main";
    cpci.layout       = pl;

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

    // ---- Push constants ----
    struct PC { uint32_t total_words, word_stride, iters, elems_per_inv, mode, seed; } pc{};
    pc.total_words   = (uint32_t)(data_bytes / 4);
    pc.word_stride   = (uint32_t)(args.stride_bytes / 4);
    pc.elems_per_inv = args.elems_per_inv;
    pc.mode          = mode;
    pc.seed          = 0x12345678u;

    const uint32_t local_size_x = 256;
    const uint64_t invocations  = (uint64_t)args.workgroups * local_size_x;

    auto bytes_for_one_iter = [&](uint32_t iters_for_dispatch)->uint64_t {
        uint64_t words_per_iter = invocations * (uint64_t)pc.elems_per_inv;
        uint64_t bytes_per_iter = words_per_iter * 4ull * ((mode == 2) ? 2ull : 1ull);
        return bytes_per_iter * (uint64_t)iters_for_dispatch;
    };

    std::printf("Config:\n");
    std::printf("  data: %u MB, mode=%s, stride=%uB, iters=%u (logical), chunk-iters=%u, einv=%u, wg=%u\n",
                args.mb, args.mode.c_str(), args.stride_bytes, args.iters, args.chunk_iters, args.elems_per_inv, args.workgroups);

    std::printf("  est bytes/chunk-dispatch (chunk-iters=%u): %.3f GB\n",
                args.chunk_iters, (double)bytes_for_one_iter(args.chunk_iters) / (1024.0*1024.0*1024.0));

    std::printf("Running %u seconds...\n", args.seconds);

    auto t0 = std::chrono::steady_clock::now();
    auto t_end = t0 + std::chrono::seconds(args.seconds);

    uint64_t chunks = 0;
    uint64_t total_est_bytes = 0;
    uint32_t current_chunk_iters = args.chunk_iters;

    auto submit_one = [&](uint32_t iters_for_dispatch)->VkResult {
        pc.iters = iters_for_dispatch;

        VK_CHECK(vkResetFences(dev, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cb, 0));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &pc);
        vkCmdDispatch(cb, args.workgroups, 1, 1);

        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;

        VkResult r1 = vkQueueSubmit(queue, 1, &si, fence);
        if (r1 != VK_SUCCESS) return r1;

        VkResult r2 = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        return r2;
    };

    // Watchdog-safe loop:
    // - Each "logical iters" is split into multiple chunk dispatches.
    // - If DEVICE_LOST occurs, we automatically reduce chunk-iters and retry (best-effort).
    while (std::chrono::steady_clock::now() < t_end) {
        uint32_t remaining = args.iters;
        while (remaining > 0 && std::chrono::steady_clock::now() < t_end) {
            uint32_t it = (remaining > current_chunk_iters) ? current_chunk_iters : remaining;

            VkResult rr = submit_one(it);
            if (rr == VK_ERROR_DEVICE_LOST) {
                if (current_chunk_iters > 1) {
                    current_chunk_iters = (current_chunk_iters + 1) / 2;
                    std::fprintf(stderr,
                        "WARN: DEVICE_LOST. Reducing chunk-iters to %u and retrying...\n",
                        current_chunk_iters);
                    // small pause to avoid immediate re-trigger
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue; // retry with smaller chunk
                } else {
                    std::fprintf(stderr, "FATAL: DEVICE_LOST even at chunk-iters=1.\n");
                    goto done;
                }
            } else if (rr != VK_SUCCESS) {
                std::fprintf(stderr, "Vulkan submit/wait failed: %s (%d)\n", vkres(rr), (int)rr);
                goto done;
            }

            pc.seed += 0x9e3779b9u;
            remaining -= it;
            chunks++;
            total_est_bytes += bytes_for_one_iter(it);
        }
    }

done:
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    double gb = (double)total_est_bytes / (1024.0*1024.0*1024.0);
    double gbps = (sec > 0.0) ? (gb / sec) : 0.0;

    std::printf("Done.\n");
    std::printf("  chunks(submits): %llu\n", (unsigned long long)chunks);
    std::printf("  time: %.3f s\n", sec);
    std::printf("  est traffic: %.3f GB\n", gb);
    std::printf("  est throughput: %.3f GB/s\n", gbps);
    std::printf("  final chunk-iters used: %u\n", current_chunk_iters);

    vkDeviceWaitIdle(dev);

    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, cmdPool, 1, &cb);
    vkDestroyCommandPool(dev, cmdPool, nullptr);

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
