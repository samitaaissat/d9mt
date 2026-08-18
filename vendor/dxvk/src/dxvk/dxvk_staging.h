#pragma once

#include <queue>

#include "dxvk_buffer.h"
#include "dxvk_device.h"

namespace dxvk {

  class DxvkCommandList;

  /**
   * \brief POD staging slice
   *
   * Result of \ref DxvkStagingBuffer::allocPod. Carries only what the
   * consumers bind and write through: no \c Rc, so returning one costs no
   * atomic refcount traffic — the point of the POD path (V2_ARCHITECTURE
   * §7 phase 1). Lifetime is NOT carried by this struct; allocPod tracks
   * the backing buffer into the command list instead.
   */
  struct StagingSlicePod {
    /// Backing buffer handle
    VkBuffer buffer = VK_NULL_HANDLE;
    /// Byte offset of the slice within the buffer
    VkDeviceSize offset = 0u;
    /// Host pointer to the start of the slice
    void* mapPtr = nullptr;
  };


  /**
   * \brief Staging buffer statistics
   *
   * Can optionally be used to throttle resource
   * uploads through the staging buffer.
   */
  struct DxvkStagingBufferStats {
    /// Total amount allocated since the buffer was created
    VkDeviceSize allocatedTotal = 0u;
    /// Amount allocated since the last time the buffer was reset
    VkDeviceSize allocatedSinceLastReset = 0u;
  };


  /**
   * \brief Staging buffer
   *
   * Provides a simple linear staging buffer
   * allocator for data uploads.
   */
  class DxvkStagingBuffer {

  public:

    /**
     * \brief Creates staging buffer
     *
     * \param [in] device DXVK device
     * \param [in] size Buffer size
     */
    DxvkStagingBuffer(
      const Rc<DxvkDevice>&     device,
            VkDeviceSize        size);

    /**
     * \brief Frees staging buffer
     */
    ~DxvkStagingBuffer();

    /**
     * \brief Allocates staging buffer memory
     *
     * Tries to suballocate from existing buffer,
     * or creates a new buffer if necessary.
     * \param [in] size Number of bytes to allocate
     * \returns Allocated slice
     */
    DxvkBufferSlice alloc(VkDeviceSize size);

    /**
     * \brief Allocates staging buffer memory, POD result
     *
     * Same allocation behavior as \ref alloc, but returns a
     * \ref StagingSlicePod instead of a \c DxvkBufferSlice, so the hot
     * per-draw callers pay no Rc copy/destroy pair per allocation.
     * Lifetime is preserved by tracking the backing buffer into
     * \c cmdList here: DxvkCommandList::track dedupes via trackId, so
     * each chunk is appended once per command-list incarnation (exactly
     * the guarantee the per-slice track() calls used to provide, at one
     * compare instead of an Rc round-trip per allocation).
     * \param [in] size Number of bytes to allocate
     * \param [in] cmdList Command list that will consume the slice
     * \returns Allocated slice, POD
     */
    StagingSlicePod allocPod(VkDeviceSize size, DxvkCommandList* cmdList);

    /**
     * \brief Resets staging buffer and allocator
     */
    void reset();

    /**
     * \brief Retrieves allocation statistics
     * \returns Current allocation statistics
     */
    DxvkStagingBufferStats getStatistics() const {
      DxvkStagingBufferStats result = { };
      result.allocatedTotal = m_allocationCounter;
      result.allocatedSinceLastReset = m_allocationCounter - m_allocationCounterValueOnReset;
      return result;
    }

  private:

    Rc<DxvkDevice>  m_device = nullptr;
    Rc<DxvkBuffer>  m_buffer = nullptr;
    VkDeviceSize    m_offset = 0u;
    VkDeviceSize    m_size = 0u;

    VkDeviceSize    m_allocationCounter = 0u;
    VkDeviceSize    m_allocationCounterValueOnReset = 0u;

  };

}
