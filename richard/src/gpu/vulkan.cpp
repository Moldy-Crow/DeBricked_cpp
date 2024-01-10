#include "gpu.hpp"
#include "exception.hpp"
#include "trace.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <vector>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <fstream>

namespace richard {
namespace gpu {

#define VK_CHECK(fnCall, msg) \
  { \
    VkResult code = fnCall; \
    if (code != VK_SUCCESS) { \
      EXCEPTION(msg << " (result: " << code << ")"); \
    } \
  }

namespace {

class SourceIncluder : public shaderc::CompileOptions::IncluderInterface {
  public:
    SourceIncluder(const std::filesystem::path& sourcesDirectory)
      : m_sourcesDirectory(sourcesDirectory) {}

    shaderc_include_result* GetInclude(const char* requested_source,
      shaderc_include_type type, const char* requesting_source, size_t include_depth) override;

    void ReleaseInclude(shaderc_include_result* data) override;

  private:
    std::filesystem::path m_sourcesDirectory;
    std::string m_errorMessage;
};

shaderc_include_result* SourceIncluder::GetInclude(const char* requested_source,
  shaderc_include_type, const char*, size_t) {

  auto result = new shaderc_include_result{};

  try {
    auto sourcePath = m_sourcesDirectory.append(requested_source);
    std::ifstream stream(sourcePath, std::ios::binary | std::ios::ate);

    ASSERT_MSG(stream.good(), "Error opening file " << sourcePath);

    size_t contentLength = stream.tellg();
    stream.seekg(0);

    char* contentBuffer = new char[contentLength];
    stream.read(contentBuffer, contentLength);

    size_t sourceNameLength = sourcePath.string().length();
    char* nameBuffer = new char[sourceNameLength];
    strcpy(nameBuffer, reinterpret_cast<const char*>(sourcePath.c_str()));

    result->source_name = nameBuffer;
    result->source_name_length = sourceNameLength;
    result->content = contentBuffer;
    result->content_length = contentLength;
    result->user_data = nullptr;
  }
  catch (const std::exception& ex) {
    m_errorMessage = ex.what();
    result->content = m_errorMessage.c_str();
    result->content_length = m_errorMessage.length();
  }

  return result;
}

void SourceIncluder::ReleaseInclude(shaderc_include_result* data) {
  if (data) {
    if (data->content) {
      delete[] data->content;
    }
    if (data->source_name) {
      delete[] data->source_name;
    }
    delete data;
  }
}

const std::vector<const char*> ValidationLayers = {
  "VK_LAYER_KHRONOS_validation"
};

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
};

struct Pipeline {
  VkPipeline handle = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  Size3 numWorkgroups = { 1, 1, 1 };
};

class Vulkan : public Gpu {
  public:
    Vulkan(Logger& logger);

    ShaderHandle compileShader(const std::string& source,
      const GpuBufferBindings& bufferBindings, const SpecializationConstants& constants,
      const Size3& workgroupSize, const Size3& numWorkgroups,
      const std::string& includesPath) override;
    GpuBuffer allocateBuffer(size_t size, GpuBufferFlags flags) override;
    void submitBufferData(GpuBufferHandle buffer, const void* data) override;
    void queueShader(ShaderHandle shaderHandle) override;
    void retrieveBuffer(GpuBufferHandle buffer, void* data) override;
    void flushQueue() override;

    ~Vulkan();

  private:
    void createVulkanInstance();
    void pickPhysicalDevice();
    void createLogicalDevice();
    uint32_t findComputeQueueFamily() const;
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
      VkBuffer& buffer, VkDeviceMemory& bufferMemory) const;
    VkDescriptorSetLayout createDescriptorSetLayout(const GpuBufferBindings& buffers);
    VkPipelineLayout createPipelineLayout(VkDescriptorSetLayout descriptorSetLayout);
    void createCommandPool();
    void createDescriptorPool();
    VkDescriptorSet createDescriptorSet(const GpuBufferBindings& buffers,
      VkDescriptorSetLayout layout);
    VkCommandBuffer createCommandBuffer();
    void dispatchWorkgroups(VkCommandBuffer commandBuffer, size_t pipelineIdx,
      const Size3& numWorkgroups);
    void createSyncObjects();
    VkShaderModule createShaderModule(const std::string& sourcePath,
      const std::string& includesPath) const;

#ifndef NDEBUG
    void setupDebugMessenger();
    void destroyDebugMessenger();
    VkDebugUtilsMessengerCreateInfoEXT getDebugMessengerCreateInfo() const;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*);
#endif

    Logger& m_logger;
    VkInstance m_instance;
    VkDebugUtilsMessengerEXT m_debugMessenger;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VkQueue m_computeQueue; // TODO: Separate queue for transfers?
    std::vector<Buffer> m_buffers;
    std::vector<Pipeline> m_pipelines;
    VkCommandPool m_commandPool;
    std::vector<VkCommandBuffer> m_commandBuffers;
    VkDescriptorPool m_descriptorPool;
    VkFence m_taskCompleteFence;
};

Vulkan::Vulkan(Logger& logger)
  : m_logger(logger) {

  createVulkanInstance();
#ifndef NDEBUG
  setupDebugMessenger();
#endif
  pickPhysicalDevice();
  createLogicalDevice();
  createCommandPool();
  createDescriptorPool();
  createSyncObjects();
}

void chooseVulkanBufferFlags(GpuBufferFlags flags, VkMemoryPropertyFlags& memProps,
  VkBufferUsageFlags& usage, VkDescriptorType& type, bool& memoryMapped) {

  if (!!(flags & GpuBufferFlags::shaderReadonly) && !(flags & GpuBufferFlags::large)) {
    type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    memProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memoryMapped = true;
  }
  else {
    type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!!(flags & GpuBufferFlags::frequentHostAccess)) {
      memoryMapped = true;
      memProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    else {
      if (!!(flags & GpuBufferFlags::hostReadAccess)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        memoryMapped = false;
      }
      if (!!(flags & GpuBufferFlags::hostWriteAccess)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        memoryMapped = false;
      }
      memProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
  }
}

GpuBuffer Vulkan::allocateBuffer(size_t size, GpuBufferFlags flags) {
  DBG_TRACE

  Buffer buffer;
  buffer.size = size;

  VkMemoryPropertyFlags memProps = 0;
  VkBufferUsageFlags usage = 0;
  bool memoryMapped = false;

  chooseVulkanBufferFlags(flags, memProps, usage, buffer.type, memoryMapped);

  GpuBuffer gpuBuffer;
  gpuBuffer.size = size;

  createBuffer(size, usage, memProps, buffer.handle, buffer.memory);
  if (memoryMapped) {
    vkMapMemory(m_device, buffer.memory, 0, buffer.size, 0,
      reinterpret_cast<void**>(&gpuBuffer.data));
  }

  m_buffers.push_back(buffer);

  // TODO: Can't just be an index if we allow buffer deletion
  gpuBuffer.handle = m_buffers.size() - 1;

  return gpuBuffer;
}

void Vulkan::submitBufferData(GpuBufferHandle bufferHandle, const void* data) {
  DBG_TRACE

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;

  Buffer& buffer = m_buffers[bufferHandle];

  VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                              | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

  VkBufferUsageFlags stagingUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  createBuffer(buffer.size, stagingUsage, flags, stagingBuffer, stagingBufferMemory);

  void* stagingBufferMapped = nullptr;
  vkMapMemory(m_device, stagingBufferMemory, 0, buffer.size, 0, &stagingBufferMapped);
  memcpy(stagingBufferMapped, data, buffer.size);
  vkUnmapMemory(m_device, stagingBufferMemory);

  copyBuffer(stagingBuffer, buffer.handle, buffer.size);
  flushQueue();

  vkFreeMemory(m_device, stagingBufferMemory, nullptr);
  vkDestroyBuffer(m_device, stagingBuffer, nullptr);
}

ShaderHandle Vulkan::compileShader(const std::string& source,
  const GpuBufferBindings& bufferBindings, const SpecializationConstants& constants,
  const Size3& workgroupSize, const Size3& numWorkgroups, const std::string& includesPath) {

  DBG_TRACE

  VkShaderModule shaderModule = createShaderModule(source, includesPath);

  std::vector<uint8_t> specializationData;
  std::vector<VkSpecializationMapEntry> entries;

  specializationData.reserve((3 + constants.size()) * sizeof(uint32_t));

  for (uint32_t i = 0; i < 3; ++i) {
    size_t offset = specializationData.size();
    entries.push_back({ i, static_cast<uint32_t>(offset), sizeof(uint32_t) });
    specializationData.resize(offset + sizeof(uint32_t));
    memcpy(specializationData.data() + offset, &workgroupSize[i], sizeof(uint32_t));
  }

  for (const auto& constant : constants) {
    uint32_t constantId = entries.size();
    size_t offset = specializationData.size();
    size_t typeSize = 4;
    switch (constant.type) {
      case SpecializationConstant::Type::float_type: {
        specializationData.resize(offset + typeSize);
        memcpy(specializationData.data() + offset, &std::get<float>(constant.value), typeSize);
        break;
      }
      case SpecializationConstant::Type::uint_type: {
        specializationData.resize(offset + typeSize);
        memcpy(specializationData.data() + offset, &std::get<uint32_t>(constant.value), typeSize);
        break;
      }
      case SpecializationConstant::Type::bool_type: {
        uint32_t value = std::get<bool>(constant.value);
        specializationData.resize(offset + typeSize);
        memcpy(specializationData.data() + offset, &value, typeSize);
        break;
      }
    }
    entries.push_back({ constantId, static_cast<uint32_t>(offset), typeSize });
  }

  const VkSpecializationInfo specializationInfo = {
    static_cast<uint32_t>(entries.size()),
    entries.data(),
    specializationData.size(),
    specializationData.data()
  };

  Pipeline pipeline;
  pipeline.numWorkgroups = numWorkgroups;
  pipeline.descriptorSetLayout = createDescriptorSetLayout(bufferBindings);
  pipeline.layout = createPipelineLayout(pipeline.descriptorSetLayout);

  VkPipelineShaderStageCreateInfo shaderStageInfo{};
  shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  shaderStageInfo.module = shaderModule;
  shaderStageInfo.pName = "main";
  shaderStageInfo.pSpecializationInfo = &specializationInfo;

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.layout = pipeline.layout;
  pipelineInfo.stage = shaderStageInfo;

  VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
    &pipeline.handle), "Failed to create compute pipeline");

  vkDestroyShaderModule(m_device, shaderModule, nullptr);

  pipeline.descriptorSet = createDescriptorSet(bufferBindings, pipeline.descriptorSetLayout);

  m_pipelines.push_back(pipeline);

  return m_pipelines.size() - 1;
}

void Vulkan::queueShader(ShaderHandle shaderHandle) {
  const Pipeline& pipeline = m_pipelines[shaderHandle];

  VkCommandBuffer commandBuffer = createCommandBuffer();
  m_commandBuffers.push_back(commandBuffer);

  dispatchWorkgroups(commandBuffer, shaderHandle, pipeline.numWorkgroups);
}

void Vulkan::flushQueue() {
  DBG_TRACE

  if (m_commandBuffers.empty()) {
    return;
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = m_commandBuffers.size();
  submitInfo.pCommandBuffers = m_commandBuffers.data();

  VK_CHECK(vkQueueSubmit(m_computeQueue, 1, &submitInfo, m_taskCompleteFence),
    "Failed to submit compute command buffer");

  VK_CHECK(vkWaitForFences(m_device, 1, &m_taskCompleteFence, VK_TRUE, UINT64_MAX),
    "Error waiting for fence");

  VK_CHECK(vkResetFences(m_device, 1, &m_taskCompleteFence), "Error resetting fence");

  vkFreeCommandBuffers(m_device, m_commandPool, m_commandBuffers.size(), m_commandBuffers.data());
  m_commandBuffers.clear();
}

void Vulkan::retrieveBuffer(GpuBufferHandle bufIdx, void* data) {
  DBG_TRACE

  Buffer& buffer = m_buffers[bufIdx];

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;

  VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                              | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

  VkBufferUsageFlags stagingUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  createBuffer(buffer.size, stagingUsage, flags, stagingBuffer, stagingBufferMemory);

  copyBuffer(buffer.handle, stagingBuffer, buffer.size);
  flushQueue();

  void* stagingBufferMapped = nullptr;
  vkMapMemory(m_device, stagingBufferMemory, 0, buffer.size, 0, &stagingBufferMapped);
  memcpy(data, stagingBufferMapped, buffer.size);
  vkUnmapMemory(m_device, stagingBufferMemory);

  vkFreeMemory(m_device, stagingBufferMemory, nullptr);
  vkDestroyBuffer(m_device, stagingBuffer, nullptr);
}

#ifndef NDEBUG
void checkValidationLayerSupport() {
  uint32_t layerCount;
  VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr),
    "Failed to enumerate instance layer properties");

  std::vector<VkLayerProperties> available(layerCount);
  VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, available.data()),
    "Failed to enumerate instance layer properties");

  for (auto layer : ValidationLayers) {
    auto fnMatches = [=](const VkLayerProperties& p) {
      return strcmp(layer, p.layerName) == 0;
    };
    if (std::find_if(available.begin(), available.end(), fnMatches) == available.end()) {
      EXCEPTION("Validation layer '" << layer << "' not supported");
    }
  }
}

VKAPI_ATTR VkBool32 VKAPI_CALL Vulkan::debugCallback(
  VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
  const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {

  Logger& logger = *reinterpret_cast<Logger*>(userData);
  logger.info(STR("Validation layer: " << data->pMessage));

  return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT Vulkan::getDebugMessengerCreateInfo() const {
  VkDebugUtilsMessengerCreateInfoEXT createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  createInfo.pfnUserCallback = debugCallback;
  createInfo.pUserData = &m_logger;
  return createInfo;
}

void Vulkan::setupDebugMessenger() {
  auto createInfo = getDebugMessengerCreateInfo();

  auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
    vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
  if (func == nullptr) {
    EXCEPTION("Error getting pointer to vkCreateDebugUtilsMessengerEXT()");
  }
  VK_CHECK(func(m_instance, &createInfo, nullptr, &m_debugMessenger),
    "Error setting up debug messenger");
}

void Vulkan::destroyDebugMessenger() {
  auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
    vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
  func(m_instance, m_debugMessenger, nullptr);
}
#endif

std::vector<const char*> getRequiredExtensions() {
  std::vector<const char*> extensions;

#ifndef NDEBUG
  extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

  return extensions;
}

void Vulkan::pickPhysicalDevice() {
  uint32_t deviceCount = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr),
    "Failed to enumerate physical devices");

  if (deviceCount == 0) {
    EXCEPTION("No physical devices found");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()),
    "Failed to enumerate physical devices");

  m_physicalDevice = devices[0];
}

uint32_t Vulkan::findComputeQueueFamily() const {
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount,
    queueFamilies.data());

  for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      return i;
    }
  }

  EXCEPTION("Could not find compute queue family");
}

void Vulkan::createLogicalDevice() {
  VkDeviceQueueCreateInfo queueCreateInfo{};

  queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueCreateInfo.queueFamilyIndex = findComputeQueueFamily();
  queueCreateInfo.queueCount = 1;
  float queuePriority = 1;
  queueCreateInfo.pQueuePriorities = &queuePriority;

  VkPhysicalDeviceFeatures deviceFeatures{};

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = 1;
  createInfo.pQueueCreateInfos = &queueCreateInfo;
  createInfo.pEnabledFeatures = &deviceFeatures;
  createInfo.enabledExtensionCount = 0;

#ifdef NDEBUG
  createInfo.enabledLayerCount = 0;
#else
  createInfo.enabledLayerCount = ValidationLayers.size();
  createInfo.ppEnabledLayerNames = ValidationLayers.data();
#endif

  VK_CHECK(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device),
    "Failed to create logical device");

  vkGetDeviceQueue(m_device, queueCreateInfo.queueFamilyIndex, 0, &m_computeQueue);
}

void Vulkan::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
  DBG_TRACE

  VkCommandBuffer commandBuffer = createCommandBuffer();

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(commandBuffer, &beginInfo);

  VkBufferCopy copyRegion{};
  copyRegion.srcOffset = 0;
  copyRegion.dstOffset = 0;
  copyRegion.size = size;
  vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

  vkEndCommandBuffer(commandBuffer);

  m_commandBuffers.push_back(commandBuffer);
}

void Vulkan::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) const {

  DBG_TRACE

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  bufferInfo.flags = 0;

  VK_CHECK(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create buffer");

  auto findMemoryType = [this, properties](uint32_t typeFilter) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
      if (typeFilter & (1 << i) &&
        (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {

        return i;
      }
    }

    EXCEPTION("Failed to find suitable memory type");
  };

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits);

  VK_CHECK(vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory),
    "Failed to allocate memory for buffer");

  vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
}

void Vulkan::createVulkanInstance() {
#ifndef NDEBUG
  checkValidationLayerSupport();
#endif

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "Vulkan Compute Examples";
  appInfo.applicationVersion = VK_MAKE_API_VERSION(1, 0, 0, 0);
  appInfo.pEngineName = "No Engine";
  appInfo.engineVersion = VK_MAKE_API_VERSION(1, 0, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
#ifdef NDEBUG
  createInfo.enabledLayerCount = 0;
  createInfo.pNext = nullptr;
#else
  createInfo.enabledLayerCount = ValidationLayers.size();
  createInfo.ppEnabledLayerNames = ValidationLayers.data();

  auto debugMessengerInfo = getDebugMessengerCreateInfo();
  createInfo.pNext = &debugMessengerInfo;
#endif

  auto extensions = getRequiredExtensions();

  createInfo.enabledExtensionCount = extensions.size();
  createInfo.ppEnabledExtensionNames = extensions.data();

  VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_instance), "Failed to create instance");
}

void Vulkan::createCommandPool() {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = findComputeQueueFamily();
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

  VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool),
    "Failed to create command pool");
}

VkCommandBuffer Vulkan::createCommandBuffer() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = m_commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;

  VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer),
    "Failed to allocate command buffer");

  return commandBuffer;
}

VkShaderModule Vulkan::createShaderModule(const std::string& source,
  const std::string& includesPath) const {

  DBG_TRACE

  shaderc::Compiler compiler;
  shaderc::CompileOptions options;

  if (!includesPath.empty()) {
    options.SetIncluder(std::make_unique<SourceIncluder>(includesPath));
  }

  auto result = compiler.CompileGlslToSpv(source, shaderc_shader_kind::shaderc_glsl_compute_shader,
    "shader", options);

  if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
    EXCEPTION("Error compiling shader: " << result.GetErrorMessage());
  }

  std::vector<uint32_t> code;
  code.assign(result.cbegin(), result.cend());

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size() * sizeof(uint32_t);
  createInfo.pCode = code.data();

  VkShaderModule shaderModule;
  VK_CHECK(vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule),
    "Failed to create shader module");

  return shaderModule;
}

VkDescriptorSetLayout Vulkan::createDescriptorSetLayout(const GpuBufferBindings& buffers) {
  DBG_TRACE

  std::vector<VkDescriptorSetLayoutBinding> bindings;

  for (uint32_t slot = 0; slot < buffers.size(); ++slot) {
    GpuBufferHandle index = buffers[slot];
    const Buffer& buffer = m_buffers[index];

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = slot;
    binding.descriptorType = buffer.type;
    binding.descriptorCount = 1;  // TODO: Support arrays of buffers
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binding.pImmutableSamplers = nullptr;

    bindings.push_back(binding);
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = bindings.size();
  layoutInfo.pBindings = bindings.data();

  VkDescriptorSetLayout layout;

  VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &layout),
    "Failed to create descriptor set layout");

  return layout;
}

void Vulkan::createDescriptorPool() {
  std::array<VkDescriptorPoolSize, 2> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 128; // TODO

  poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[1].descriptorCount = 32; // TODO

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = poolSizes.size();
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = 32; // TODO

  VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool),
    "Failed to create descriptor pool");
}

VkDescriptorSet Vulkan::createDescriptorSet(const GpuBufferBindings& buffers,
  VkDescriptorSetLayout layout) {

  DBG_TRACE

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = m_descriptorPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &layout;

  VkDescriptorSet descriptorSet;

  VK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &descriptorSet),
    "Failed to allocate descriptor set");

  std::vector<VkDescriptorBufferInfo> bufferInfos(buffers.size());
  std::vector<VkWriteDescriptorSet> descriptorWrites(buffers.size());

  for (size_t slot = 0; slot < buffers.size(); ++slot) {
    GpuBufferHandle bufIdx = buffers[slot];
    const Buffer& buffer = m_buffers[bufIdx];

    auto& bufferInfo = bufferInfos[slot];
    bufferInfo.buffer = buffer.handle;
    bufferInfo.offset = 0;
    bufferInfo.range = buffer.size;

    auto& descriptorWrite = descriptorWrites[slot];
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = slot;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = buffer.type;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;
    descriptorWrite.pImageInfo = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
  }

  vkUpdateDescriptorSets(m_device, descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);

  return descriptorSet;
}

VkPipelineLayout Vulkan::createPipelineLayout(VkDescriptorSetLayout descriptorSetLayout) {
  DBG_TRACE

  VkPipelineLayout pipelineLayout;

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 0;  // TODO: Support push constants
  pipelineLayoutInfo.pPushConstantRanges = nullptr;
  VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipelineLayout),
    "Failed to create pipeline layout");

  return pipelineLayout;
}

void Vulkan::dispatchWorkgroups(VkCommandBuffer commandBuffer, size_t pipelineIdx,
  const Size3& numWorkgroups) {

  DBG_TRACE

  const Pipeline& pipeline = m_pipelines[pipelineIdx];

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = 0;
  beginInfo.pInheritanceInfo = nullptr;

  VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo),
    "Failed to begin recording command buffer");

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1,
    &pipeline.descriptorSet, 0, 0);
  vkCmdDispatch(commandBuffer, numWorkgroups[0], numWorkgroups[1], numWorkgroups[2]);

  VK_CHECK(vkEndCommandBuffer(commandBuffer), "Failed to record command buffer");
}

void Vulkan::createSyncObjects() {
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = 0;

  VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_taskCompleteFence),
    "Failed to create fence");
}

Vulkan::~Vulkan() {
  vkDestroyFence(m_device, m_taskCompleteFence, nullptr);
  vkDestroyCommandPool(m_device, m_commandPool, nullptr);
  for (const auto& pipeline : m_pipelines) {
    vkDestroyPipeline(m_device, pipeline.handle, nullptr);
    vkDestroyPipelineLayout(m_device, pipeline.layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, pipeline.descriptorSetLayout, nullptr);
  }
  for (auto& buffer : m_buffers) {
    vkDestroyBuffer(m_device, buffer.handle, nullptr);
    vkFreeMemory(m_device, buffer.memory, nullptr);
  }
  vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
#ifndef NDEBUG
  destroyDebugMessenger();
#endif
  vkDestroyDevice(m_device, nullptr);
  vkDestroyInstance(m_instance, nullptr);
}

}

GpuPtr createGpu(Logger& logger) {
  return std::make_unique<Vulkan>(logger);
}

}
}
