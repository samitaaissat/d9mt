// d9mt: link stubs for the DXVK v2.7.1 d3d9 front-end build (d3d9fe.dll).
//
// Every backend symbol (DxvkContext / DxvkDevice / resources / presenter /
// ...) referenced by the vendored front-end resolves here to a loud abort.
// PE ld resolves symbols BEFORE --gc-sections, so even dead-path references
// need definitions.
//
// Pattern: include the real vendored headers and define the declared
// methods (no mangled-name hacks).  Bodies log the calling symbol and
// abort().  These get replaced one class at a time by the real Metal
// backend.

#include <cstdio>
#include <cstdlib>

#include "../../vendor/dxvk/src/dxvk/dxvk_access.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_adapter.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_buffer.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_cmdlist.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_context.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_gpu_event.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_gpu_query.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_image.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_instance.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_latency.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_memory.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_pipemanager.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_presenter.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_queue.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_sampler.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_sparse.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_staging.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_swapchain_blitter.h"
#include "../../vendor/dxvk/src/dxvk/hud/dxvk_hud.h"
#include "../../vendor/dxvk/src/vulkan/vulkan_loader.h"
#include "../../vendor/dxvk/src/wsi/wsi_edid.h"

#define D9MT_STUB_BODY                                                        \
  {                                                                           \
    std::fprintf(stderr, "d3d9fe stub hit: %s\n", __PRETTY_FUNCTION__);       \
    std::fflush(stderr);                                                      \
    std::abort();                                                             \
  }

namespace dxvk {

  // -------------------------------------------------------------- instance
  DxvkInstance::DxvkInstance(DxvkInstanceFlags flags) D9MT_STUB_BODY
  DxvkInstance::~DxvkInstance() D9MT_STUB_BODY
  Rc<DxvkAdapter> DxvkInstance::enumAdapters(uint32_t index) const D9MT_STUB_BODY

  // --------------------------------------------------------------- adapter
  DxvkAdapter::~DxvkAdapter() D9MT_STUB_BODY
  Rc<vk::InstanceFn> DxvkAdapter::vki() const D9MT_STUB_BODY
  DxvkFormatFeatures DxvkAdapter::getFormatFeatures(VkFormat format) const D9MT_STUB_BODY
  std::optional<DxvkFormatLimits> DxvkAdapter::getFormatLimits(
    const DxvkFormatQuery& query) const D9MT_STUB_BODY
  Rc<DxvkDevice> DxvkAdapter::createDevice() D9MT_STUB_BODY
  bool DxvkAdapter::matchesDriver(VkDriverIdKHR driver) const D9MT_STUB_BODY
  bool DxvkAdapter::matchesDriver(VkDriverIdKHR driver,
    Version minVer, Version maxVer) const D9MT_STUB_BODY

  // ---------------------------------------------------------------- device
  DxvkDevice::~DxvkDevice() D9MT_STUB_BODY
  bool DxvkDevice::canUseGraphicsPipelineLibrary() const D9MT_STUB_BODY
  bool DxvkDevice::mustTrackPipelineLifetime() const D9MT_STUB_BODY
  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const D9MT_STUB_BODY
  Rc<DxvkCommandList> DxvkDevice::createCommandList() D9MT_STUB_BODY
  Rc<DxvkContext> DxvkDevice::createContext() D9MT_STUB_BODY
  Rc<DxvkEvent> DxvkDevice::createGpuEvent() D9MT_STUB_BODY
  Rc<DxvkQuery> DxvkDevice::createGpuQuery(
    VkQueryType type, VkQueryControlFlags flags, uint32_t index) D9MT_STUB_BODY
  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
    VkMemoryPropertyFlags memoryType) D9MT_STUB_BODY
  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo& createInfo,
    VkMemoryPropertyFlags memoryType) D9MT_STUB_BODY
  Rc<DxvkSampler> DxvkDevice::createSampler(const DxvkSamplerKey& key) D9MT_STUB_BODY
  const DxvkPipelineLayout* DxvkDevice::createBuiltInPipelineLayout(
    DxvkPipelineLayoutFlags flags, VkShaderStageFlags pushDataStages,
    VkDeviceSize pushDataSize, uint32_t bindingCount,
    const DxvkDescriptorSetLayoutBinding* bindings) D9MT_STUB_BODY
  VkPipeline DxvkDevice::createBuiltInComputePipeline(
    const DxvkPipelineLayout* layout,
    const util::DxvkBuiltInShaderStage& stage) D9MT_STUB_BODY
  void DxvkDevice::registerShader(const Rc<DxvkShader>& shader) D9MT_STUB_BODY
  void DxvkDevice::requestCompileShader(const Rc<DxvkShader>& shader) D9MT_STUB_BODY
  Rc<DxvkLatencyTracker> DxvkDevice::createLatencyTracker(
    const Rc<Presenter>& presenter) D9MT_STUB_BODY
  void DxvkDevice::presentImage(const Rc<Presenter>& presenter,
    const Rc<DxvkLatencyTracker>& tracker, uint64_t frameId,
    DxvkSubmitStatus* status) D9MT_STUB_BODY
  VkResult DxvkDevice::waitForSubmission(DxvkSubmitStatus* status) D9MT_STUB_BODY
  void DxvkDevice::waitForFence(sync::Fence& fence, uint64_t value) D9MT_STUB_BODY
  void DxvkDevice::waitForResource(const DxvkPagedResource& resource,
    DxvkAccess access) D9MT_STUB_BODY
  void DxvkDevice::waitForIdle() D9MT_STUB_BODY

  // --------------------------------------------------------------- context
  DxvkContext::~DxvkContext() D9MT_STUB_BODY
  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) D9MT_STUB_BODY
  void DxvkContext::endFrame() D9MT_STUB_BODY
  void DxvkContext::beginLatencyTracking(const Rc<DxvkLatencyTracker>& tracker,
    uint64_t frameId) D9MT_STUB_BODY
  void DxvkContext::endLatencyTracking(const Rc<DxvkLatencyTracker>& tracker) D9MT_STUB_BODY
  void DxvkContext::flushCommandList(const VkDebugUtilsLabelEXT* reason,
    DxvkSubmitStatus* status) D9MT_STUB_BODY
  Rc<DxvkCommandList> DxvkContext::beginExternalRendering() D9MT_STUB_BODY
  void DxvkContext::beginQuery(const Rc<DxvkQuery>& query) D9MT_STUB_BODY
  void DxvkContext::endQuery(const Rc<DxvkQuery>& query) D9MT_STUB_BODY
  void DxvkContext::blitImageView(const Rc<DxvkImageView>& dstView,
    const VkOffset3D* dstOffsets, const Rc<DxvkImageView>& srcView,
    const VkOffset3D* srcOffsets, VkFilter filter) D9MT_STUB_BODY
  void DxvkContext::changeImageLayout(const Rc<DxvkImage>& image,
    VkImageLayout layout) D9MT_STUB_BODY
  void DxvkContext::clearRenderTarget(const Rc<DxvkImageView>& imageView,
    VkImageAspectFlags clearAspects, VkClearValue clearValue,
    VkImageAspectFlags discardAspects) D9MT_STUB_BODY
  void DxvkContext::clearImageView(const Rc<DxvkImageView>& imageView,
    VkOffset3D offset, VkExtent3D extent, VkImageAspectFlags aspect,
    VkClearValue value) D9MT_STUB_BODY
  void DxvkContext::copyBuffer(const Rc<DxvkBuffer>& dstBuffer,
    VkDeviceSize dstOffset, const Rc<DxvkBuffer>& srcBuffer,
    VkDeviceSize srcOffset, VkDeviceSize numBytes) D9MT_STUB_BODY
  void DxvkContext::copyBufferToImage(const Rc<DxvkImage>& dstImage,
    VkImageSubresourceLayers dstSubresource, VkOffset3D dstOffset,
    VkExtent3D dstExtent, const Rc<DxvkBuffer>& srcBuffer,
    VkDeviceSize srcOffset, VkDeviceSize rowAlignment,
    VkDeviceSize sliceAlignment, VkFormat srcFormat) D9MT_STUB_BODY
  void DxvkContext::copyImage(const Rc<DxvkImage>& dstImage,
    VkImageSubresourceLayers dstSubresource, VkOffset3D dstOffset,
    const Rc<DxvkImage>& srcImage, VkImageSubresourceLayers srcSubresource,
    VkOffset3D srcOffset, VkExtent3D extent) D9MT_STUB_BODY
  void DxvkContext::copyImageToBuffer(const Rc<DxvkBuffer>& dstBuffer,
    VkDeviceSize dstOffset, VkDeviceSize rowAlignment,
    VkDeviceSize sliceAlignment, VkFormat dstFormat,
    const Rc<DxvkImage>& srcImage, VkImageSubresourceLayers srcSubresource,
    VkOffset3D srcOffset, VkExtent3D srcExtent) D9MT_STUB_BODY
  void DxvkContext::draw(uint32_t count,
    const VkDrawIndirectCommand* draws) D9MT_STUB_BODY
  void DxvkContext::drawIndexed(uint32_t count,
    const VkDrawIndexedIndirectCommand* draws) D9MT_STUB_BODY
  void DxvkContext::emitGraphicsBarrier(VkPipelineStageFlags srcStages,
    VkAccessFlags srcAccess, VkPipelineStageFlags dstStages,
    VkAccessFlags dstAccess) D9MT_STUB_BODY
  void DxvkContext::generateMipmaps(const Rc<DxvkImageView>& imageView,
    VkFilter filter) D9MT_STUB_BODY
  void DxvkContext::initBuffer(const Rc<DxvkBuffer>& buffer) D9MT_STUB_BODY
  void DxvkContext::initImage(const Rc<DxvkImage>& image,
    VkImageLayout initialLayout) D9MT_STUB_BODY
  void DxvkContext::invalidateBuffer(const Rc<DxvkBuffer>& buffer,
    Rc<DxvkResourceAllocation>&& slice) D9MT_STUB_BODY
  bool DxvkContext::ensureImageCompatibility(const Rc<DxvkImage>& image,
    const DxvkImageUsageInfo& usageInfo) D9MT_STUB_BODY
  void DxvkContext::resolveImage(const Rc<DxvkImage>& dstImage,
    const Rc<DxvkImage>& srcImage, const VkImageResolve& region,
    VkFormat format, VkResolveModeFlagBits mode,
    VkResolveModeFlagBits stencilMode) D9MT_STUB_BODY
  void DxvkContext::transformImage(const Rc<DxvkImage>& dstImage,
    const VkImageSubresourceRange& dstSubresources, VkImageLayout srcLayout,
    VkImageLayout dstLayout) D9MT_STUB_BODY
  void DxvkContext::setViewports(uint32_t viewportCount,
    const DxvkViewport* viewports) D9MT_STUB_BODY
  void DxvkContext::setBlendConstants(DxvkBlendConstants blendConstants) D9MT_STUB_BODY
  void DxvkContext::setDepthBias(DxvkDepthBias depthBias) D9MT_STUB_BODY
  void DxvkContext::setDepthBiasRepresentation(
    DxvkDepthBiasRepresentation depthBiasRepresentation) D9MT_STUB_BODY
  void DxvkContext::setDepthBounds(DxvkDepthBounds depthBounds) D9MT_STUB_BODY
  void DxvkContext::setStencilReference(uint32_t reference) D9MT_STUB_BODY
  void DxvkContext::setInputAssemblyState(const DxvkInputAssemblyState& ia) D9MT_STUB_BODY
  void DxvkContext::setInputLayout(uint32_t attributeCount,
    const DxvkVertexInput* attributes, uint32_t bindingCount,
    const DxvkVertexInput* bindings) D9MT_STUB_BODY
  void DxvkContext::setRasterizerState(const DxvkRasterizerState& rs) D9MT_STUB_BODY
  void DxvkContext::setMultisampleState(const DxvkMultisampleState& ms) D9MT_STUB_BODY
  void DxvkContext::setDepthStencilState(const DxvkDepthStencilState& ds) D9MT_STUB_BODY
  void DxvkContext::setLogicOpState(const DxvkLogicOpState& lo) D9MT_STUB_BODY
  void DxvkContext::setBlendMode(uint32_t attachment,
    const DxvkBlendMode& blendMode) D9MT_STUB_BODY
  void DxvkContext::signalGpuEvent(const Rc<DxvkEvent>& event) D9MT_STUB_BODY
  void DxvkContext::writeTimestamp(const Rc<DxvkQuery>& query) D9MT_STUB_BODY
  void DxvkContext::signal(const Rc<sync::Signal>& signal, uint64_t value) D9MT_STUB_BODY
  void DxvkContext::beginDebugLabel(const VkDebugUtilsLabelEXT& label) D9MT_STUB_BODY
  void DxvkContext::endDebugLabel() D9MT_STUB_BODY
  void DxvkContext::insertDebugLabel(const VkDebugUtilsLabelEXT& label) D9MT_STUB_BODY

  // ----------------------------------------------------------- command list
  DxvkCommandList::~DxvkCommandList() D9MT_STUB_BODY
  void DxvkCommandList::bindResources(DxvkCmdBuffer cmdBuffer,
    const DxvkPipelineLayout* layout, uint32_t descriptorCount,
    const DxvkDescriptorWrite* descriptorInfos, size_t pushDataSize,
    const void* pushData) D9MT_STUB_BODY

  // -------------------------------------------------------------- resources
  Rc<DxvkBufferView> DxvkBuffer::createView(const DxvkBufferViewKey& info) D9MT_STUB_BODY
  void DxvkBufferView::updateViews() D9MT_STUB_BODY
  Rc<DxvkImageView> DxvkImage::createView(const DxvkImageViewKey& info) D9MT_STUB_BODY
  HANDLE DxvkImage::sharedHandle() const D9MT_STUB_BODY
  const DxvkDescriptor* DxvkImageView::createView(VkImageViewType type) const D9MT_STUB_BODY
  void DxvkImageView::updateViews() D9MT_STUB_BODY
  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::createBufferResource(
    const VkBufferCreateInfo& createInfo,
    const DxvkAllocationInfo& allocationInfo,
    DxvkLocalAllocationCache* allocationCache) D9MT_STUB_BODY
  void DxvkMemoryAllocator::freeAllocation(
    DxvkResourceAllocation* allocation) D9MT_STUB_BODY
  void DxvkObjectTracker::advanceList() D9MT_STUB_BODY
  DxvkDescriptorUpdateList::DxvkDescriptorUpdateList(DxvkDevice* device,
    uint32_t setSize, uint32_t descriptorCount,
    const DxvkDescriptorUpdateInfo* descriptorInfos) D9MT_STUB_BODY
  DxvkDescriptorUpdateList::~DxvkDescriptorUpdateList() D9MT_STUB_BODY
  DxvkResourceRef::~DxvkResourceRef() D9MT_STUB_BODY
  void DxvkSampler::release() D9MT_STUB_BODY
  DxvkSamplerDescriptorSet DxvkSamplerDescriptorHeap::getDescriptorSetInfo() const D9MT_STUB_BODY
  DxvkStagingBuffer::DxvkStagingBuffer(const Rc<DxvkDevice>& device,
    VkDeviceSize size) D9MT_STUB_BODY
  DxvkStagingBuffer::~DxvkStagingBuffer() D9MT_STUB_BODY
  DxvkBufferSlice DxvkStagingBuffer::alloc(VkDeviceSize size) D9MT_STUB_BODY

  // ------------------------------------------------------ pipeline manager
  const DxvkDescriptorSetLayout* DxvkPipelineManager::createDescriptorSetLayout(
    const DxvkDescriptorSetLayoutKey& key) D9MT_STUB_BODY
  const DxvkPipelineLayout* DxvkPipelineManager::createPipelineLayout(
    const DxvkPipelineLayoutKey& key) D9MT_STUB_BODY

  // -------------------------------------------------------- queries/events
  DxvkQuery::~DxvkQuery() D9MT_STUB_BODY
  DxvkGpuQueryStatus DxvkQuery::getData(DxvkQueryData& queryData) D9MT_STUB_BODY
  DxvkEvent::~DxvkEvent() D9MT_STUB_BODY
  DxvkGpuEventStatus DxvkEvent::test() D9MT_STUB_BODY

  // ------------------------------------------------------ submission queue
  void DxvkSubmissionQueue::synchronize() D9MT_STUB_BODY
  void DxvkSubmissionQueue::lockDeviceQueue() D9MT_STUB_BODY
  void DxvkSubmissionQueue::unlockDeviceQueue() D9MT_STUB_BODY

  // --------------------------------------------------------------- present
  Presenter::Presenter(const Rc<DxvkDevice>& device,
    const Rc<sync::Signal>& signal, const PresenterDesc& desc,
    PresenterSurfaceProc&& proc) D9MT_STUB_BODY
  Presenter::~Presenter() D9MT_STUB_BODY
  VkResult Presenter::acquireNextImage(PresenterSync& sync,
    Rc<DxvkImage>& image) D9MT_STUB_BODY
  void Presenter::setSyncInterval(uint32_t syncInterval) D9MT_STUB_BODY
  void Presenter::setFrameRateLimit(double frameRate, uint32_t maxLatency) D9MT_STUB_BODY
  void Presenter::setSurfaceFormat(VkSurfaceFormatKHR format) D9MT_STUB_BODY
  void Presenter::setSurfaceExtent(VkExtent2D extent) D9MT_STUB_BODY
  void Presenter::setHdrMetadata(VkHdrMetadataEXT hdrMetadata) D9MT_STUB_BODY
  bool Presenter::supportsColorSpace(VkColorSpaceKHR colorspace) D9MT_STUB_BODY
  void Presenter::invalidateSurface() D9MT_STUB_BODY
  void Presenter::destroyResources() D9MT_STUB_BODY

  DxvkSwapchainBlitter::DxvkSwapchainBlitter(const Rc<DxvkDevice>& device,
    const Rc<hud::Hud>& hud) D9MT_STUB_BODY
  DxvkSwapchainBlitter::~DxvkSwapchainBlitter() D9MT_STUB_BODY
  void DxvkSwapchainBlitter::present(const Rc<DxvkCommandList>& ctx,
    const Rc<DxvkImageView>& dstView, VkRect2D dstRect,
    const Rc<DxvkImageView>& srcView, VkRect2D srcRect) D9MT_STUB_BODY
  void DxvkSwapchainBlitter::setGammaRamp(uint32_t cpCount,
    const DxvkGammaCp* cpData) D9MT_STUB_BODY
  void DxvkSwapchainBlitter::setCursorTexture(VkExtent2D extent,
    VkFormat format, const void* data) D9MT_STUB_BODY
  void DxvkSwapchainBlitter::setCursorPos(VkRect2D rect) D9MT_STUB_BODY

  // ------------------------------------------------------------------- hud
  namespace hud {
    Hud::~Hud() D9MT_STUB_BODY
    Rc<Hud> Hud::createHud(const Rc<DxvkDevice>& device) D9MT_STUB_BODY
    HudItem::~HudItem() D9MT_STUB_BODY
    void HudItem::update(dxvk::high_resolution_clock::time_point time) D9MT_STUB_BODY
    HudClientApiItem::HudClientApiItem(std::string api) D9MT_STUB_BODY
    HudClientApiItem::~HudClientApiItem() D9MT_STUB_BODY
    HudPos HudClientApiItem::render(const Rc<DxvkCommandList>& ctx,
      const HudPipelineKey& key, const HudOptions& options,
      HudRenderer& renderer, HudPos position) D9MT_STUB_BODY
    HudLatencyItem::HudLatencyItem() D9MT_STUB_BODY
    HudLatencyItem::~HudLatencyItem() D9MT_STUB_BODY
    void HudLatencyItem::accumulateStats(const DxvkLatencyStats& stats) D9MT_STUB_BODY
    void HudLatencyItem::update(dxvk::high_resolution_clock::time_point time) D9MT_STUB_BODY
    HudPos HudLatencyItem::render(const Rc<DxvkCommandList>& ctx,
      const HudPipelineKey& key, const HudOptions& options,
      HudRenderer& renderer, HudPos position) D9MT_STUB_BODY
    void HudRenderer::drawText(uint32_t size, HudPos pos, uint32_t color,
      const std::string& text) D9MT_STUB_BODY
  } // namespace hud

  // ---------------------------------------------------------------- vulkan
  namespace vk {
    LibraryFn::~LibraryFn() D9MT_STUB_BODY
    InstanceFn::~InstanceFn() D9MT_STUB_BODY
    DeviceFn::~DeviceFn() D9MT_STUB_BODY
  } // namespace vk

  // ------------------------------------------------------------------- wsi
  namespace wsi {
    // d9mt: real (non-abort) impl per BACKEND-SURFACE.md §6.2 — replaces
    // wsi_edid.cpp, which hard-depends on libdisplay-info.
    std::optional<WsiDisplayMetadata> parseColorimetryInfo(
      const std::vector<uint8_t>& edid) {
      return std::nullopt;
    }
  } // namespace wsi

} // namespace dxvk
