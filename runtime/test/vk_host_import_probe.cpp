// vk_host_import_probe: can the console title's own memory back Vulkan buffers without copies?
//
// The title reserves a large range as placeholders and maps 64 KiB physical pages (pagefile-backed section views,
// as runtime/shims/xmem.cpp makes them) into it piece by piece, then asks for one GPU buffer over the whole range at
// that address (device slot 133). This checks, on the local GPU:
//   A  committed VirtualAlloc memory imports with VK_EXT_external_memory_host (the baseline);
//   B  a 64 KiB section view mapped into a placeholder imports too;
//   C  a sparse buffer over the range takes the imported page at the matching offset (vkQueueBindSparse);
//   D  CPU writes are seen by the GPU through that buffer, and GPU writes by the CPU, with no copy of ours;
//   E  the same page mapped at a second address imports again and aliases the first;
//   F  a large committed region imports in one piece (how big).
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// VirtualAlloc2 and MapViewOfFile3 come from onecore (CMake links it).

namespace {

// The loader is opened at run time, as the other vk_* tools do (no import library at build time).
PFN_vkCreateInstance vkCreateInstance;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
PFN_vkCreateDevice vkCreateDevice;
PFN_vkGetDeviceQueue vkGetDeviceQueue;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
PFN_vkAllocateMemory vkAllocateMemory;
PFN_vkFreeMemory vkFreeMemory;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkDestroyBuffer vkDestroyBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkBindBufferMemory vkBindBufferMemory;
PFN_vkMapMemory vkMapMemory;
PFN_vkUnmapMemory vkUnmapMemory;
PFN_vkQueueBindSparse vkQueueBindSparse;
PFN_vkQueueWaitIdle vkQueueWaitIdle;
PFN_vkQueueSubmit vkQueueSubmit;
PFN_vkCreateCommandPool vkCreateCommandPool;
PFN_vkDestroyCommandPool vkDestroyCommandPool;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
PFN_vkEndCommandBuffer vkEndCommandBuffer;
PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
PFN_vkCmdFillBuffer vkCmdFillBuffer;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
PFN_vkDestroyDevice vkDestroyDevice;
PFN_vkDestroyInstance vkDestroyInstance;

bool load_global() {
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll");
    if (!dll) return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vkGetInstanceProcAddr"));
    if (!gipa) return false;
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    return vkCreateInstance != nullptr;
}

bool load_instance(VkInstance instance) {
    HMODULE dll = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vkGetInstanceProcAddr"));
    bool ok = true;
    ok &= (vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"))) != nullptr;
    ok &= (vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"))) != nullptr;
    ok &= (vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"))) != nullptr;
    ok &= (vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"))) != nullptr;
    ok &= (vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))) != nullptr;
    ok &= (vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))) != nullptr;
    ok &= (vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"))) != nullptr;
    ok &= (vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))) != nullptr;
    ok &= (vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"))) != nullptr;
    ok &= (vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"))) != nullptr;
    ok &= (vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"))) != nullptr;
    ok &= (vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(gipa(instance, "vkFreeMemory"))) != nullptr;
    ok &= (vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(gipa(instance, "vkCreateBuffer"))) != nullptr;
    ok &= (vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(gipa(instance, "vkDestroyBuffer"))) != nullptr;
    ok &= (vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gipa(instance, "vkGetBufferMemoryRequirements"))) != nullptr;
    ok &= (vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"))) != nullptr;
    ok &= (vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(gipa(instance, "vkMapMemory"))) != nullptr;
    ok &= (vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(gipa(instance, "vkUnmapMemory"))) != nullptr;
    ok &= (vkQueueBindSparse = reinterpret_cast<PFN_vkQueueBindSparse>(gipa(instance, "vkQueueBindSparse"))) != nullptr;
    ok &= (vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))) != nullptr;
    ok &= (vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))) != nullptr;
    ok &= (vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(gipa(instance, "vkCreateCommandPool"))) != nullptr;
    ok &= (vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(gipa(instance, "vkDestroyCommandPool"))) != nullptr;
    ok &= (vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))) != nullptr;
    ok &= (vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))) != nullptr;
    ok &= (vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))) != nullptr;
    ok &= (vkCmdCopyBuffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(gipa(instance, "vkCmdCopyBuffer"))) != nullptr;
    ok &= (vkCmdFillBuffer = reinterpret_cast<PFN_vkCmdFillBuffer>(gipa(instance, "vkCmdFillBuffer"))) != nullptr;
    ok &= (vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(instance, "vkCmdPipelineBarrier"))) != nullptr;
    ok &= (vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gipa(instance, "vkDestroyDevice"))) != nullptr;
    ok &= (vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))) != nullptr;
    return ok;
}

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mem{};
    VkDeviceSize host_align = 0;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_props{};
};

constexpr size_t kPage = 0x10000;

uint32_t pick_type(const Vk& vk, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vk.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool init(Vk& vk, const char* want_name) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &vk.instance) != VK_SUCCESS || !load_instance(vk.instance)) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(vk.instance, &n, devs.data());
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        // LEONIDA_VK_DEVICE picks by part of the name, without case; unset, the first that is not Microsoft's Dozen.
        const bool match = *want_name ? lower(p.deviceName).find(lower(want_name)) != std::string::npos
                                      : std::strstr(p.deviceName, "Microsoft") == nullptr;
        if (match) {
            vk.phys = d;
            std::printf("device: %s (driver 0x%x, api %u.%u)\n", p.deviceName, p.driverVersion,
                        VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
            break;
        }
    }
    if (!vk.phys) return false;
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(vk.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts_have(en);
    vkEnumerateDeviceExtensionProperties(vk.phys, nullptr, &en, exts_have.data());
    bool has_host = false;
    for (const auto& e : exts_have) has_host |= std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
    if (!has_host) return false;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
    vkGetPhysicalDeviceProperties2(vk.phys, &p2);
    vk.host_align = host.minImportedHostPointerAlignment;
    std::printf("minImportedHostPointerAlignment: 0x%llx, sparseAddressSpaceSize: 0x%llx\n",
                static_cast<unsigned long long>(vk.host_align),
                static_cast<unsigned long long>(p2.properties.limits.sparseAddressSpaceSize));
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures(vk.phys, &f);
    std::printf("sparseBinding %u, sparseResidencyBuffer %u\n", f.sparseBinding, f.sparseResidencyBuffer);
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i)
        if ((qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            vk.family = i;
            break;
        }
    if (vk.family == UINT32_MAX) return false;
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = vk.family;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    const char* exts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkPhysicalDeviceFeatures enable{};
    enable.sparseBinding = f.sparseBinding;
    enable.sparseResidencyBuffer = f.sparseResidencyBuffer;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    dci.pEnabledFeatures = &enable;
    if (vkCreateDevice(vk.phys, &dci, nullptr, &vk.device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    vk.host_props = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(vk.device, "vkGetMemoryHostPointerPropertiesEXT"));
    return vk.host_props != nullptr;
}

// Imports `size` bytes at `ptr` as device memory; VK_NULL_HANDLE (with the result printed) on failure.
VkDeviceMemory import(const Vk& vk, void* ptr, VkDeviceSize size, uint32_t allowed_bits, const char* what) {
    VkMemoryHostPointerPropertiesEXT props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vk.host_props(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &props);
    if (r != VK_SUCCESS || !props.memoryTypeBits) {
        std::printf("     %s: vkGetMemoryHostPointerPropertiesEXT %d, bits 0x%x\n", what, r, props.memoryTypeBits);
        return VK_NULL_HANDLE;
    }
    const uint32_t type = pick_type(vk, props.memoryTypeBits & allowed_bits, 0);
    std::printf("     %s: host pointer memory types 0x%x (using %u, flags 0x%x)\n", what, props.memoryTypeBits, type,
                type < 32 ? vk.mem.memoryTypes[type].propertyFlags : 0);
    if (type == UINT32_MAX) return VK_NULL_HANDLE;
    VkImportMemoryHostPointerInfoEXT host{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    host.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    host.pHostPointer = ptr;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &host};
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory m{};
    r = vkAllocateMemory(vk.device, &ai, nullptr, &m);
    if (r != VK_SUCCESS) std::printf("     %s: vkAllocateMemory %d\n", what, r);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkBuffer make_buffer(const Vk& vk, VkDeviceSize size, bool sparse, bool external) {
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &ext : nullptr};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (sparse) bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    VkBuffer b{};
    return vkCreateBuffer(vk.device, &bi, nullptr, &b) == VK_SUCCESS ? b : VK_NULL_HANDLE;
}

// Records `record` into a one-off command buffer and waits for it.
template <typename F>
bool submit(const Vk& vk, F record) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = vk.family;
    VkCommandPool pool{};
    vkCreateCommandPool(vk.device, &pi, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    vkAllocateCommandBuffers(vk.device, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    const bool ok = vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(vk.queue) == VK_SUCCESS;
    vkDestroyCommandPool(vk.device, pool, nullptr);
    return ok;
}

// A 64 KiB pagefile-backed section, as xmem.cpp's physical pages.
HANDLE make_section(size_t bytes) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, static_cast<DWORD>(bytes),
                              nullptr);
}

// Reserves `bytes` as one placeholder and returns its base.
void* reserve_placeholder(size_t bytes) {
    return VirtualAlloc2(nullptr, nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
}

// Maps `section` (offset 0, kPage bytes) at `at`, splitting the placeholder there first.
void* map_page_into(void* at, HANDLE section) {
    VirtualFree(at, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    return MapViewOfFile3(section, nullptr, at, 0, kPage, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
    Vk vk;
    bool sizes = false;
    const char* name = "";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sizes") == 0) sizes = true;
        else name = argv[i];
    }
    char env[128] = {};
    if (!*name && GetEnvironmentVariableA("LEONIDA_VK_DEVICE", env, sizeof(env))) name = env;
    if (!load_global()) {
        std::printf("SKIP: no Vulkan loader (vulkan-1.dll)\n");
        return 0;
    }
    if (!init(vk, name)) {
        std::printf("SKIP: no device%s%s with VK_EXT_external_memory_host, sparse buffers and a sparse-binding "
                    "graphics queue\n", *name ? " matching " : "", name);
        return 0;
    }
    const uint32_t all = ~0u;

    // A: committed VirtualAlloc.
    std::printf("\n-- A: committed VirtualAlloc --\n");
    void* committed = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VkDeviceMemory a = import(vk, committed, kPage, all, "64 KiB committed");
    check(a != VK_NULL_HANDLE, "A: committed memory imports");

    // B: a section view in a placeholder, as the title's physical pages.
    std::printf("\n-- B: section view in a placeholder --\n");
    constexpr size_t kRange = 64ull << 20;  // the title's pool is larger; 64 MiB is enough to place pages in
    auto* range = static_cast<uint8_t*>(reserve_placeholder(kRange));
    check(range != nullptr, "B: a 64 MiB placeholder reservation");
    HANDLE section = make_section(kPage);
    constexpr size_t kAt = 5 * kPage;  // a page in the middle of the range
    auto* view = static_cast<uint8_t*>(range ? map_page_into(range + kAt, section) : nullptr);
    check(view == range + kAt, "B: the section view lands in the placeholder");
    VkDeviceMemory b = view ? import(vk, view, kPage, all, "section view") : VK_NULL_HANDLE;
    check(b != VK_NULL_HANDLE, "B: a section view in a placeholder imports");

    // C: a sparse buffer over the range takes the page at its offset.
    std::printf("\n-- C: sparse buffer over the range --\n");
    VkBuffer sparse = make_buffer(vk, kRange, true, true);
    check(sparse != VK_NULL_HANDLE, "C: a sparse buffer with external host memory over the range");
    bool bound = false;
    if (sparse && b) {
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk.device, sparse, &req);
        std::printf("     sparse buffer alignment 0x%llx, memory types 0x%x\n",
                    static_cast<unsigned long long>(req.alignment), req.memoryTypeBits);
        VkSparseMemoryBind bind{};
        bind.resourceOffset = kAt;
        bind.size = kPage;
        bind.memory = b;
        VkSparseBufferMemoryBindInfo bbi{sparse, 1, &bind};
        VkBindSparseInfo bsi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        bsi.bufferBindCount = 1;
        bsi.pBufferBinds = &bbi;
        const VkResult r = vkQueueBindSparse(vk.queue, 1, &bsi, VK_NULL_HANDLE);
        bound = r == VK_SUCCESS && vkQueueWaitIdle(vk.queue) == VK_SUCCESS;
        if (r != VK_SUCCESS) std::printf("     vkQueueBindSparse %d\n", r);
    }
    check(bound, "C: the imported page binds into the sparse buffer at its offset");

    // D: CPU -> GPU and GPU -> CPU through the sparse buffer, with no copy of ours.
    std::printf("\n-- D: coherence through the sparse buffer --\n");
    if (bound) {
        for (size_t i = 0; i < kPage / 4; ++i) reinterpret_cast<uint32_t*>(view)[i] = 0xC0DE0000u + static_cast<uint32_t>(i);
        // A host-visible readback buffer (ordinary memory) for the GPU's copy.
        VkBuffer readback = make_buffer(vk, kPage, false, false);
        VkMemoryRequirements rr{};
        vkGetBufferMemoryRequirements(vk.device, readback, &rr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = rr.size;
        ai.memoryTypeIndex = pick_type(vk, rr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory rm{};
        vkAllocateMemory(vk.device, &ai, nullptr, &rm);
        vkBindBufferMemory(vk.device, readback, rm, 0);
        const bool copied = submit(vk, [&](VkCommandBuffer cb) {
            VkBufferCopy c{kAt, 0, kPage};
            vkCmdCopyBuffer(cb, sparse, readback, 1, &c);
        });
        void* mapped = nullptr;
        vkMapMemory(vk.device, rm, 0, kPage, 0, &mapped);
        check(copied && std::memcmp(mapped, view, kPage) == 0, "D: the GPU reads what the CPU wrote into the page");
        vkUnmapMemory(vk.device, rm);

        const bool filled = submit(vk, [&](VkCommandBuffer cb) { vkCmdFillBuffer(cb, sparse, kAt + 0x100, 0x200, 0x5A5A5A5Au); });
        bool seen = filled;
        for (size_t i = 0x100; i < 0x300; ++i) seen &= view[i] == 0x5A;
        seen &= view[0xff] != 0x5A && view[0x300] != 0x5A;
        check(seen, "D: the CPU reads what the GPU wrote, exactly in the filled bytes");

        // E: the same page at a second address aliases the first.
        std::printf("\n-- E: the same page at a second address --\n");
        auto* second = static_cast<uint8_t*>(map_page_into(range + 9 * kPage, section));
        check(second && second[0x100] == 0x5A && reinterpret_cast<uint32_t*>(second)[0] == 0xC0DE0000u,
              "E: a second view of the section shows the same bytes");
        VkDeviceMemory e = second ? import(vk, second, kPage, all, "second view") : VK_NULL_HANDLE;
        check(e != VK_NULL_HANDLE, "E: the second view imports as well");
        if (e) vkFreeMemory(vk.device, e, nullptr);
        vkDestroyBuffer(vk.device, readback, nullptr);
        vkFreeMemory(vk.device, rm, nullptr);
    }

    // F: how large a committed region imports in one piece.
    // F commits up to 8 GiB, so it runs only when asked (--sizes); it reports, it does not fail.
    if (sizes) std::printf("\n-- F: large committed regions --\n");
    for (size_t mib : {256ull, 1024ull, 4095ull, 4096ull, 8192ull}) {
        if (!sizes) break;
        const size_t bytes = mib << 20;
        void* big = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!big) {
            std::printf("     %zu MiB: VirtualAlloc failed\n", mib);
            continue;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%zu MiB committed", mib);
        VkDeviceMemory m = import(vk, big, bytes, all, label);
        std::printf("     %s: %s\n", label, m ? "imports" : "refused");
        if (m) vkFreeMemory(vk.device, m, nullptr);
        VirtualFree(big, 0, MEM_RELEASE);
    }

    std::printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
