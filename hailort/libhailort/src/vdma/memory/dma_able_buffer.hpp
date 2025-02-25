/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file dma_able_buffer.hpp
 * @brief A Buffer that can be mapped to some device for dma operations.
 *        There are several options for that buffer:
 *          1. No allocation - The user gives its own buffer pointer and address. The buffer must be page aligned.
 *          2. Normal allocation - page aligned allocation. This is the default option for linux and windows.
 *          3. Driver allocation - On some platforms, default user mode memory allocation is not DMAAble. To overcome
 *             this, we allocate the buffer in a low memory using hailort driver. We check it querying
 *             HailoRTDriver::allocate_driver_buffer().
 *          4. QNX shared memory allocation - for qnx, in order to pass the driver to the resources manager, we need to
 *             create a shared memory object, and pass an handle to it in the mapping. TODO: HRT-10298 implement this.
 **/

#ifndef _HAILO_DMA_ABLE_BUFFER_HPP_
#define _HAILO_DMA_ABLE_BUFFER_HPP_

#include "hailo/expected.hpp"
#include "os/hailort_driver.hpp"
#include "os/mmap_buffer.hpp"

namespace hailort {
namespace vdma {

class DmaAbleBuffer;
using DmaAbleBufferPtr = std::shared_ptr<DmaAbleBuffer>;

class DmaAbleBuffer {
public:
    // If user_address is not nullptr, allocation is not needed.
    static Expected<DmaAbleBufferPtr> create(size_t size, void *user_address = nullptr);

    // The driver is used only if driver.allocate_driver_buffer is true, and that the user address is nullptr.
    static Expected<DmaAbleBufferPtr> create(HailoRTDriver &driver, size_t size, void *user_address = nullptr);

    DmaAbleBuffer() = default;
    DmaAbleBuffer(DmaAbleBuffer &&other) = delete;
    DmaAbleBuffer(const DmaAbleBuffer &other) = delete;
    DmaAbleBuffer &operator=(const DmaAbleBuffer &other) = delete;
    DmaAbleBuffer &operator=(DmaAbleBuffer &&other) = delete;
    virtual ~DmaAbleBuffer() = default;

    virtual void* user_address() = 0;
    virtual size_t size() const = 0;
    virtual vdma_mapped_buffer_driver_identifier buffer_identifier() = 0;
};

} /* namespace vdma */
} /* namespace hailort */

#endif /* _HAILO_DMA_ABLE_BUFFER_HPP_ */
