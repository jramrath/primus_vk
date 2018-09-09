#include "vulkan.h"
#include "vk_layer.h"

#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>

#include <assert.h>
#include <string.h>

#include <mutex>
#include <map>
#include <vector>
#include <iostream>

#include <pthread.h>

#include <stdexcept>

#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
  return *(void **)inst;
}

std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
std::map<void *, VkLayerDispatchTable> device_dispatch;
std::map<void *, VkPhysicalDevice> render_to_display;
std::map<void *, VkDevice> render_to_display_instance;

VkInstance the_instance;

#define TRACE(x) std::cout << "PrimusVK: " << x;
#define TRACE_PROFILING(x) std::cout << "PrimusVK: " << x;
// #define TRACE(x)
#define TRACE_FRAME(x)

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown
VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice);
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
  TRACE("CreateInstance\n");
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);
  
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerInstanceDispatchTable dispatchTable;
#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);
  FORWARD(GetInstanceProcAddr);
  FORWARD(EnumeratePhysicalDevices);
  FORWARD(DestroyInstance);
  FORWARD(EnumerateDeviceExtensionProperties);
  FORWARD(GetPhysicalDeviceProperties);
#undef FORWARD
  
  std::vector<VkPhysicalDevice> physicalDevices;
  {
    auto enumerateDevices = dispatchTable.EnumeratePhysicalDevices;
    TRACE("Getting devices" << std::endl);
    uint32_t gpuCount = 0;
    enumerateDevices(*pInstance, &gpuCount, nullptr);
    physicalDevices.resize(gpuCount);
    enumerateDevices(*pInstance, &gpuCount, physicalDevices.data());
  }
  VkPhysicalDevice display = VK_NULL_HANDLE;
  VkPhysicalDevice render = VK_NULL_HANDLE;
  for(auto &dev: physicalDevices){
    VkPhysicalDeviceProperties props;
    dispatchTable.GetPhysicalDeviceProperties(dev, &props);
    TRACE(GetKey(dev) << ": ");
    if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){
      display = dev;
      TRACE("got display!\n");
    }
    if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
      TRACE("got render!\n");
      render = dev;
    }
    TRACE("Device: " << props.deviceName << std::endl);
    TRACE("  Type: " << props.deviceType << std::endl);
  }
  if(display == VK_NULL_HANDLE || render == VK_NULL_HANDLE){
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  render_to_display[GetKey(render)] = display;
  TRACE(GetKey(render) << " --> " << GetKey(display) << "\n");
  the_instance = *pInstance;

#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);
  FORWARD(GetPhysicalDeviceSurfaceFormatsKHR);
  FORWARD(GetPhysicalDeviceQueueFamilyProperties);
  FORWARD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  FORWARD(GetPhysicalDeviceSurfaceSupportKHR);
  FORWARD(GetPhysicalDeviceSurfacePresentModesKHR);
#undef FORWARD
  
  // store the table by key
  {
    scoped_lock l(global_lock);
    instance_dispatch[GetKey(*pInstance)] = dispatchTable;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  instance_dispatch.erase(GetKey(instance));
}

#include <vector>
#include <memory>
#include <thread>

//#define VK_CHECK_RESULT(x) do{ const VkResult r = x; if(r != VK_SUCCESS){printf("Error %d in %d\n", 7, __LINE__);}}while(0);
#define VK_CHECK_RESULT(x) if(x != VK_SUCCESS){printf("Error %d, in %d\n", x, __LINE__);}
struct FramebufferImage;
struct MappedMemory{
  VkDevice device;
  VkDeviceMemory mem;
  char* data;
  MappedMemory(VkDevice device, FramebufferImage &img);
  ~MappedMemory();
};
struct FramebufferImage {
  VkImage img;
  VkDeviceMemory mem;

  VkDevice device;

  std::shared_ptr<MappedMemory> mapped;
  FramebufferImage(VkDevice device, VkExtent2D size, VkImageTiling tiling, VkImageUsageFlags usage, int memoryTypeIndex): device(device){
    VkImageCreateInfo imageCreateCI {};
    imageCreateCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
    imageCreateCI.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageCreateCI.extent.width = size.width;
    imageCreateCI.extent.height = size.height;
    imageCreateCI.extent.depth = 1;
    imageCreateCI.arrayLayers = 1;
    imageCreateCI.mipLevels = 1;
    imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateCI.tiling = tiling;
    imageCreateCI.usage = usage;
    VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &img));

    VkMemoryRequirements memRequirements {};
    VkMemoryAllocateInfo memAllocInfo {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    vkGetImageMemoryRequirements(device, img, &memRequirements);
    memAllocInfo.allocationSize = memRequirements.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &mem));
    VK_CHECK_RESULT(vkBindImageMemory(device, img, mem, 0));
  }
  std::shared_ptr<MappedMemory> getMapped(){
    if(!mapped){
      throw std::runtime_error("not mapped");
    }
    return mapped;
  }
  void map(){
    mapped = std::make_shared<MappedMemory>(device, *this);
  }
  VkSubresourceLayout getLayout(){
    VkImageSubresource subResource { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout subResourceLayout;
    vkGetImageSubresourceLayout(device, img, &subResource, &subResourceLayout);
    return subResourceLayout;
  }
  ~FramebufferImage(){
    vkFreeMemory(device, mem, nullptr);
    vkDestroyImage(device, img, nullptr);
  }
};
MappedMemory::MappedMemory(VkDevice device, FramebufferImage &img): device(device), mem(img.mem){
  vkMapMemory(device, img.mem, 0, VK_WHOLE_SIZE, 0, (void**)&data);
}
MappedMemory::~MappedMemory(){
  vkUnmapMemory(device, mem);
}
class CommandBuffer;
struct MySwapchain{
  VkDevice device;
  VkQueue render_queue;
  VkDevice display_device;
  VkQueue display_queue;
  VkSwapchainKHR backend;
  std::vector<std::shared_ptr<FramebufferImage>> render_images;
  std::vector<std::shared_ptr<FramebufferImage>> render_copy_images;
  std::vector<std::shared_ptr<FramebufferImage>> display_src_images;
  std::vector<VkImage> display_images;
  VkExtent2D imgSize;

  std::vector<std::shared_ptr<CommandBuffer>> display_commands;

  void copyImageData(uint32_t idx);
  std::shared_ptr<MappedMemory> storeImage(uint32_t index, VkDevice device, VkExtent2D imgSize, VkQueue queue, VkFormat colorFormat);
};

bool list_all_gpus = false;
void* threadmain(void *d);
class CreateOtherDevice {
public:
  VkPhysicalDevice display_dev;
  VkPhysicalDevice render_dev;
  VkPhysicalDeviceMemoryProperties display_mem;
  VkPhysicalDeviceMemoryProperties render_mem;
  VkDevice render_gpu;
  VkDevice display_gpu;
  CreateOtherDevice(VkPhysicalDevice display_dev, VkPhysicalDevice render_dev, VkDevice render_gpu):
    display_dev(display_dev), render_dev(render_dev), render_gpu(render_gpu){
  }
  void run(){
    VkDevice pDeviceLogic;
    TRACE("Thread running\n");
    TRACE("getting rendering suff: " << GetKey(display_dev) << "\n");
    uint32_t gpuCount;
    list_all_gpus = true;
    vkEnumeratePhysicalDevices(the_instance, &gpuCount, nullptr);
    TRACE("Gpus: " << gpuCount << "\n");
    std::vector<VkPhysicalDevice> physicalDevices(gpuCount);
    vkEnumeratePhysicalDevices(the_instance, &gpuCount, physicalDevices.data());
    list_all_gpus = false;

    display_dev = physicalDevices[1];
    TRACE("phys[1]: " << display_dev << "\n");

    vkGetPhysicalDeviceMemoryProperties(display_dev, &display_mem);
    vkGetPhysicalDeviceMemoryProperties(physicalDevices[0], &render_mem);
    
    std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    if(true){
      PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueues = vkGetPhysicalDeviceQueueFamilyProperties;
      uint32_t queueFamilyCount;
      getQueues(display_dev, &queueFamilyCount, nullptr);
      assert(queueFamilyCount > 0);
      queueFamilyProperties.resize(queueFamilyCount);
      getQueues(display_dev, &queueFamilyCount, queueFamilyProperties.data());
      TRACE("render queues: " << queueFamilyCount << "\n");
      for(auto &props : queueFamilyProperties){
	TRACE(" flags: " << queueFamilyProperties[0].queueFlags << "\n");
      }
    }
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    const float defaultQueuePriority(0.0f);
    queueInfo.pQueuePriorities = &defaultQueuePriority;

    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = 1;
    const char *swap[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    createInfo.ppEnabledExtensionNames = swap;
    VkResult ret;
    
    TRACE("Creating Graphics: " << ret << ", "  << "\n");
    ret = vkCreateDevice(display_dev, &createInfo, nullptr, &pDeviceLogic);
    TRACE("Create Graphics FINISHED!: " << ret << "\n");
    TRACE("Display: " << GetKey(pDeviceLogic) << ".\n");
    TRACE("storing as reference to: " << GetKey(render_gpu)  << "\n");
    display_gpu = pDeviceLogic;

  }
  pthread_t thread;
  void start(){

    if(pthread_create(&thread, NULL, threadmain, this)) {
      fprintf(stderr, "Error creating thread\n");
    }
  }
  bool joined = false;
  void join(){
    if(joined) { TRACE( "Refusing second join" ); return; }
    int error;
    if(error = pthread_join(thread, nullptr)){
      fprintf(stderr, "Error joining thread: %d\n", error);
    }
    joined = true;
  }
};
void* threadmain(void *d){
  auto *p = reinterpret_cast<CreateOtherDevice*>(d);
  p->run();
  return nullptr;
}



class CommandBuffer {
  VkCommandPool commandPool;
  VkDevice device;
public:
  VkCommandBuffer cmd;
  CommandBuffer(VkDevice device) : device(device) {
    VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    VK_CHECK_RESULT(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo cmdBufAllocateInfo = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=commandPool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  
    VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &cmd));
  
    VkCommandBufferBeginInfo cmdBufInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK_RESULT(vkBeginCommandBuffer(cmd, &cmdBufInfo));
  }
  ~CommandBuffer(){
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
  }
  void insertImageMemoryBarrier(
			      VkImage image,
			      VkAccessFlags srcAccessMask,
			      VkAccessFlags dstAccessMask,
			      VkImageLayout oldImageLayout,
			      VkImageLayout newImageLayout,
			      VkPipelineStageFlags srcStageMask,
			      VkPipelineStageFlags dstStageMask,
			      VkImageSubresourceRange subresourceRange) {
    VkImageMemoryBarrier imageMemoryBarrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageMemoryBarrier.srcAccessMask = srcAccessMask;
    imageMemoryBarrier.dstAccessMask = dstAccessMask;
    imageMemoryBarrier.oldLayout = oldImageLayout;
    imageMemoryBarrier.newLayout = newImageLayout;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange = subresourceRange;
    
    vkCmdPipelineBarrier(
			 cmd,
			 srcStageMask,
			 dstStageMask,
			 0,
			 0, nullptr,
			 0, nullptr,
			 1, &imageMemoryBarrier);
  }
  void copyImage(VkImage src, VkImage dst, VkExtent2D imgSize){
    VkImageCopy imageCopyRegion{};
    imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.srcSubresource.layerCount = 1;
    imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.dstSubresource.layerCount = 1;
    imageCopyRegion.extent.width = imgSize.width;
    imageCopyRegion.extent.height = imgSize.height;
    imageCopyRegion.extent.depth = 1;
 
    // Issue the copy command
    vkCmdCopyImage(
		   cmd,
		   src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		   1,
		   &imageCopyRegion);
  }
  void end(){
    VK_CHECK_RESULT(vkEndCommandBuffer(cmd));
  }
  void submit(VkQueue queue, VkFence fence){
    VkSubmitInfo submitInfo = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // Submit to the queue
    VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
  }
};
class Fence{
  VkDevice device;
public:
  VkFence fence;
  Fence(VkDevice dev): device(dev){
    // Create fence to ensure that the command buffer has finished executing
    VkFenceCreateInfo fenceInfo = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=0};
    VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
  }
  void await(){
    // Wait for the fence to signal that command buffer has finished executing
    VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000L));
  }
  ~Fence(){
    vkDestroyFence(device, fence, nullptr);
  }
};


CreateOtherDevice *cod;
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
  TRACE("in function: creating device\n");
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  
  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  // store info for subsequent create call
  const auto targetLayerInfo = layerCreateInfo->u.pLayerInfo;
  VkDevice pDeviceLogic = *pDevice;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

  VkDeviceCreateInfo ifo = *pCreateInfo;
  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  {
    scoped_lock l(global_lock);
    TRACE("spawning secondary device creation\n");
    static bool first = true;
    if(first){
      // hopefully the first createFunc has only modified this one field
      layerCreateInfo->u.pLayerInfo = targetLayerInfo;
      auto display_dev = render_to_display[GetKey(physicalDevice)];
      cod = new CreateOtherDevice{display_dev, physicalDevice, *pDevice};
      cod->start();
      pthread_yield();
      first = false;
    }
  }

  // store the table by key
  {
    scoped_lock l(global_lock);
    device_dispatch[GetKey(*pDevice)] = fetchDispatchTable(gdpa, pDevice);
  }
  TRACE("CreateDevice done\n");

  return VK_SUCCESS;

}
VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice){
  TRACE("fetching dispatch for " << GetKey(*pDevice) << "\n");
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
  dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
  dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
  dispatchTable.BeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(*pDevice, "vkBeginCommandBuffer");
  dispatchTable.CmdDraw = (PFN_vkCmdDraw)gdpa(*pDevice, "vkCmdDraw");
  dispatchTable.CmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(*pDevice, "vkCmdDrawIndexed");
  dispatchTable.EndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(*pDevice, "vkEndCommandBuffer");

  dispatchTable.CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(*pDevice, "vkCreateSwapchainKHR");
  TRACE("Create Swapchain KHR is: " << (void*) dispatchTable.CreateSwapchainKHR << "\n");
  dispatchTable.DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(*pDevice, "vkDestroySwapchainKHR");
  dispatchTable.GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)gdpa(*pDevice, "vkGetSwapchainImagesKHR");
  dispatchTable.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(*pDevice, "vkAcquireNextImageKHR");
  dispatchTable.QueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");

  dispatchTable.CreateImage = (PFN_vkCreateImage)gdpa(*pDevice, "vkCreateImage");

  return dispatchTable;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  device_dispatch.erase(GetKey(device));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
  {
    scoped_lock l(global_lock);
    if(cod == nullptr){
      std::cerr << "no thread to join\n";
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    TRACE("joining secondary device creation\n");
    cod->join();
    TRACE("joining succeeded\n");
  }
  VkDevice render_gpu = device;
  VkSwapchainCreateInfoKHR info2 = *pCreateInfo;
  info2.minImageCount = 3;
  pCreateInfo = &info2;
  VkSwapchainKHR old = pCreateInfo->oldSwapchain;
  if(old != VK_NULL_HANDLE){
    MySwapchain *ch = reinterpret_cast<MySwapchain*>(old);
    info2.oldSwapchain = ch->backend;
    TRACE("Old Swapchain: " << ch->backend << "\n");
  }
  TRACE("MinImageCount: " << pCreateInfo->minImageCount << "\n");
  TRACE("fetching device for: " << GetKey(render_gpu)  << "\n");
  VkDevice display_gpu = cod->display_gpu;
  TRACE("found: " << GetKey(display_gpu) << "\n");
  
  printf("FamilyIndexCount: %d\n", pCreateInfo->queueFamilyIndexCount);
  TRACE("Dev: " << GetKey(display_gpu) << "\n";);
  TRACE("Swapchainfunc: " << (void*) device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR << "\n";);

  MySwapchain *ch = new MySwapchain();
  ch->display_device = display_gpu;
  // TODO automatically find correct queue and not choose 0 forcibly
  vkGetDeviceQueue(render_gpu, 0, 0, &ch->render_queue);
  vkGetDeviceQueue(display_gpu, 0, 0, &ch->display_queue);
  ch->device = render_gpu;
  ch->render_images.resize(pCreateInfo->minImageCount);
  ch->render_copy_images.resize(pCreateInfo->minImageCount);
  ch->display_src_images.resize(pCreateInfo->minImageCount);
  ch->display_commands.resize(pCreateInfo->minImageCount);
  ch->imgSize = pCreateInfo->imageExtent;

  VkMemoryPropertyFlags host_mem = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryPropertyFlags local_mem = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  ssize_t render_host_mem = -1;
  ssize_t render_local_mem = -1;
  ssize_t display_host_mem = -1;
  for(size_t j=0; j < cod->render_mem.memoryTypeCount; j++){
    if ( render_host_mem == -1 && ( cod->render_mem.memoryTypes[j].propertyFlags & host_mem ) == host_mem ) {
      render_host_mem = j;
    }
    if ( render_local_mem == -1 && ( cod->render_mem.memoryTypes[j].propertyFlags & local_mem ) == local_mem ) {
      render_local_mem = j;
    }
  }
  for(size_t j=0; j < cod->display_mem.memoryTypeCount; j++){
    if ( display_host_mem == -1 && ( cod->display_mem.memoryTypes[j].propertyFlags & host_mem ) == host_mem ) {
      display_host_mem = j;
    }
  }
  TRACE("Selected render mem: " << render_host_mem << ";" << render_local_mem << " display: " << display_host_mem << "\n");
  size_t i = 0;
  for( auto &renderImage: ch->render_images){
    renderImage = std::make_shared<FramebufferImage>(render_gpu, pCreateInfo->imageExtent,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT, render_local_mem);
    auto &renderCopyImage = ch->render_copy_images[i];
    renderCopyImage = std::make_shared<FramebufferImage>(render_gpu, pCreateInfo->imageExtent,
	VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, render_host_mem);
    auto &displaySrcImage = ch->display_src_images[i++];
    displaySrcImage = std::make_shared<FramebufferImage>(display_gpu, pCreateInfo->imageExtent,
	VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT, display_host_mem);

    renderCopyImage->map();
    displaySrcImage->map();

    CommandBuffer cmd{ch->display_device};
    cmd.insertImageMemoryBarrier(
			   displaySrcImage->img,
			   0,
			   VK_ACCESS_MEMORY_WRITE_BIT,
			   VK_IMAGE_LAYOUT_UNDEFINED,
			   VK_IMAGE_LAYOUT_GENERAL,
			   VK_PIPELINE_STAGE_TRANSFER_BIT,
			   VK_PIPELINE_STAGE_TRANSFER_BIT,
			   VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.end();
    Fence f{ch->display_device};
    cmd.submit(ch->display_queue, f.fence);
    f.await();
  }
  *pSwapchain = reinterpret_cast<VkSwapchainKHR>(ch);


  VkResult rc = device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR(display_gpu, pCreateInfo, pAllocator, &ch->backend);
  TRACE(">> Swapchain create done " << rc << ";" << (void*) ch->backend << "\n");

  uint32_t count;
  device_dispatch[GetKey(ch->display_device)].GetSwapchainImagesKHR(ch->display_device, ch->backend, &count, nullptr);
  TRACE("Image aquiring: " << count << "; created: " << i << "\n");
  ch->display_images.resize(count);
  device_dispatch[GetKey(ch->display_device)].GetSwapchainImagesKHR(ch->display_device, ch->backend, &count, ch->display_images.data());
  return rc;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    if(swapchain == nullptr) { return;}
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);
  TRACE(">> Destroy swapchain: " << (void*) ch->backend << "\n");
  // TODO: the Nvidia driver segfaults when passing a chain here?
  // device_dispatch[GetKey(device)].DestroySwapchainKHR(device, ch->backend, pAllocator);
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);

  *pSwapchainImageCount = ch->render_images.size();
  VkResult res = VK_SUCCESS;
  if(pSwapchainImages != nullptr) {
    printf("Get Swapchain Images buffer: %d\n", pSwapchainImages);
    res = VK_SUCCESS; //device_dispatch[GetKey(device)].GetSwapchainImagesKHR(device, ch->backend, pSwapchainImageCount, pSwapchainImages);
    for(size_t i = 0; i < *pSwapchainImageCount; i++){
      pSwapchainImages[i] = ch->render_images[i]->img;
    }
    printf("Count: %d\n", *pSwapchainImageCount);
  }
  return res;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
  TRACE_FRAME("AcquireNextImage: sem: " << semaphore << ", fence: " << fence << "\n");
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);

  VkResult res;
  {
    Fence myfence{ch->display_device};

    res = device_dispatch[GetKey(ch->display_device)].AcquireNextImageKHR(ch->display_device, ch->backend, timeout, nullptr, myfence.fence, pImageIndex);
    TRACE_FRAME("AcquireNextImageKHR: " << *pImageIndex << ";" << res << "\n");

    myfence.await();
  }
  VkSubmitInfo qsi{};
  qsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  qsi.signalSemaphoreCount = 1;
  qsi.pSignalSemaphores = &semaphore;
  vkQueueSubmit(ch->render_queue, 1, &qsi, nullptr);
  TRACE_FRAME("out: " << res << "\n");
  return res;
}
#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>

std::shared_ptr<MappedMemory> MySwapchain::storeImage(uint32_t index, VkDevice device, VkExtent2D imgSize, VkQueue queue, VkFormat colorFormat){
  auto cpyImage = render_copy_images[index];
  auto srcImage = render_images[index]->img;
    CommandBuffer cmd{device};
    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	0,					VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_MEMORY_READ_BIT,		VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  
    cmd.copyImage(srcImage, cpyImage->img, imgSize);

    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	VK_ACCESS_TRANSFER_WRITE_BIT,		VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_TRANSFER_READ_BIT,		VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

    cmd.end();
    Fence f{device};
    cmd.submit(queue, f.fence);
    f.await();
     
    VkSubresourceLayout subResourceLayout = cpyImage->getLayout();
    // Map image memory so we can start copying from it
    auto mapped = cpyImage->getMapped();
    const char* data = mapped->data + subResourceLayout.offset;
    return mapped;
}
#include <chrono>

void MySwapchain::copyImageData(uint32_t index){
  auto target = storeImage(index, device, imgSize, render_queue, VK_FORMAT_B8G8R8A8_UNORM);

  {
    auto mapped = display_src_images[index]->getMapped();

    auto start = std::chrono::steady_clock::now();
    memcpy(mapped->data, target->data, 4*imgSize.width*imgSize.height);
    TRACE_PROFILING("Time for plain memcpy: " << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count() << " seconds\n");
  }

  if(display_commands[index] == nullptr){
    display_commands[index] = std::make_shared<CommandBuffer>(display_device);
    CommandBuffer &cmd = *display_commands[index];
  cmd.insertImageMemoryBarrier(
	display_src_images[index]->img,
	VK_ACCESS_MEMORY_WRITE_BIT,	VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_GENERAL,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	 VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.insertImageMemoryBarrier(
	display_images[index],
	VK_ACCESS_MEMORY_READ_BIT,	VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.copyImage(display_src_images[index]->img, display_images[index], imgSize);

  cmd.insertImageMemoryBarrier(
	display_src_images[index]->img,
	VK_ACCESS_TRANSFER_READ_BIT,	VK_ACCESS_MEMORY_WRITE_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.insertImageMemoryBarrier(
	display_images[index],
	VK_ACCESS_TRANSFER_READ_BIT,	VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  cmd.end();

  }
  
  Fence f{display_device};
  display_commands[index]->submit(display_queue, f.fence);
  f.await();

}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  const auto start = std::chrono::steady_clock::now();

  MySwapchain *ch = reinterpret_cast<MySwapchain*>(pPresentInfo->pSwapchains[0]);

  VkPresentInfoKHR p2 = *pPresentInfo;
  p2.pSwapchains = &ch->backend;
  p2.swapchainCount = 1;
  //p2.pWaitSemaphores 
  p2.waitSemaphoreCount = 0;
  
  VkSubmitInfo qsi{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
  VkPipelineStageFlags flags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  qsi.pWaitDstStageMask = &flags;
  qsi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
  qsi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
  vkQueueSubmit(queue, 1, &qsi, nullptr);

  const auto index = pPresentInfo->pImageIndices[0];
  ch->copyImageData(index);
  
  TRACE_FRAME("Swapchain QueuePresent: #semaphores: " << pPresentInfo->waitSemaphoreCount << ", #chains: " << pPresentInfo->swapchainCount << ", imageIndex: " << *pPresentInfo->pImageIndices << "\n");
  TRACE_PROFILING("Own time for present: " << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count() << " seconds\n");
  VkResult res = device_dispatch[GetKey(ch->display_queue)].QueuePresentKHR(ch->display_queue, &p2);
  return res;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
  printf("Fetching function...\n");
  PFN_vkCreateXcbSurfaceKHR fn = (PFN_vkCreateXcbSurfaceKHR)instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR");
  printf("Xcb create surface: %d\n", fn);
  return fn(instance, pCreateInfo, pAllocator, pSurface);
}


VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
  VkPhysicalDevice phy = render_to_display[GetKey(physicalDevice)];
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, pSurfaceFormatCount, pSurfaceFormats);
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
  VkPhysicalDevice phy = physicalDevice;
  instance_dispatch[GetKey(phy)].GetPhysicalDeviceQueueFamilyProperties(phy, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = render_to_display[GetKey(physicalDevice)];
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilitiesKHR(phy, surface, pSurfaceCapabilities);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported) {
  VkPhysicalDevice phy = render_to_display[GetKey(physicalDevice)];
  queueFamilyIndex = 0;
  auto res = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceSupportKHR(phy, queueFamilyIndex, surface, pSupported);
  printf("Support: %xd, %d\n", GetKey(phy), *pSupported);
  return res;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
  VkPhysicalDevice phy = render_to_display[GetKey(physicalDevice)];
  auto res = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, pPresentModeCount, pPresentModes);
  return res;
}


///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                       VkLayerProperties *pProperties)
{
  if(pPropertyCount) *pPropertyCount = 1;

  if(pProperties)
  {
    strcpy(pProperties->layerName, "VK_LAYER_PRIMUS_PrimusVK");
    strcpy(pProperties->description, "Primus-vk - https://github.com/felixdoerre/primus_vk");
    pProperties->implementationVersion = 1;
    pProperties->specVersion = VK_API_VERSION_1_0;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
  return PrimusVK_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_PRIMUS_PrimusVK"))
    return VK_ERROR_LAYER_NOT_PRESENT;

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateDeviceExtensionProperties(
                                     VkPhysicalDevice physicalDevice, const char *pLayerName,
                                     uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  // pass through any queries that aren't to us
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_PRIMUS_PrimusVK"))
  {
    if(physicalDevice == VK_NULL_HANDLE)
      return VK_SUCCESS;

    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumeratePhysicalDevices(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceCount,
    VkPhysicalDevice*                           pPhysicalDevices){
  int cnt = 1;
  if(list_all_gpus) cnt = 2;
  if(pPhysicalDevices == nullptr){
    *pPhysicalDeviceCount = cnt;
    return VK_SUCCESS;
  }
  scoped_lock l(global_lock);
  VkResult res = instance_dispatch[GetKey(instance)].EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, nullptr);
  if(res != VK_SUCCESS) return res;
  std::vector<VkPhysicalDevice> vec{*pPhysicalDeviceCount};
  res = instance_dispatch[GetKey(instance)].EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, vec.data());
  if(res != VK_SUCCESS) return res;
  pPhysicalDevices[0] = vec[0];
  *pPhysicalDeviceCount = cnt;
  if(cnt >= 2){
    pPhysicalDevices[1] = vec[1];
  }
  return res;
}


///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer

#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&PrimusVK_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL PrimusVK_GetDeviceProcAddr(VkDevice device, const char *pName)
{
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);

  GETPROCADDR(CreateSwapchainKHR);
  GETPROCADDR(DestroySwapchainKHR);
  GETPROCADDR(GetSwapchainImagesKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
  GETPROCADDR(GetPhysicalDeviceQueueFamilyProperties);
  GETPROCADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
  GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);
  GETPROCADDR(CreateXcbSurfaceKHR);
  
  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL PrimusVK_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(EnumeratePhysicalDevices);
  GETPROCADDR(EnumerateInstanceLayerProperties);
  GETPROCADDR(EnumerateInstanceExtensionProperties);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);
  
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);

  GETPROCADDR(CreateSwapchainKHR);
  GETPROCADDR(DestroySwapchainKHR);
  GETPROCADDR(GetSwapchainImagesKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
  GETPROCADDR(GetPhysicalDeviceQueueFamilyProperties);
  GETPROCADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
  GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);
  // GETPROCADDR(CreateXcbSurfaceKHR);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}