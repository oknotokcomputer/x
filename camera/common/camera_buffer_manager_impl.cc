/*
 * Copyright 2017 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common/camera_buffer_manager_impl.h"

#include <cerrno>
#include <map>
#include <utility>
#include <vector>

#include <gbm.h>
#include <linux/videodev2.h>
#include <sys/mman.h>

#include <base/check.h>
#include <base/check_op.h>
#include <base/no_destructor.h>
#include <drm_fourcc.h>
#include <hardware/gralloc.h>
#include <system/graphics.h>

#include "common/camera_buffer_handle.h"
#include "cros-camera/common.h"
#include "hardware_buffer/allocator.h"
#include "hardware_buffer/minigbm_allocator.h"

namespace cros {

namespace {

std::map<uint32_t, std::vector<uint32_t>> kSupportedHalFormats{
    {HAL_PIXEL_FORMAT_BLOB, {DRM_FORMAT_R8}},
    {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
     {DRM_FORMAT_NV12, DRM_FORMAT_XBGR8888, DRM_FORMAT_MTISP_SXYZW10}},
    {HAL_PIXEL_FORMAT_RGBX_8888, {DRM_FORMAT_XBGR8888}},
    {HAL_PIXEL_FORMAT_YCbCr_420_888, {DRM_FORMAT_NV12}},
    // Map to DRM_FORMAT_ABGR8888 because DRM_FORMAT_VYUY or DRM_FORMAT_YUYV is
    // not generally supported by minigbm.
    {HAL_PIXEL_FORMAT_YCbCr_422_I, {DRM_FORMAT_ABGR8888}},
    {HAL_PIXEL_FORMAT_YCBCR_P010, {DRM_FORMAT_P010}},
    {HAL_PIXEL_FORMAT_Y8, {DRM_FORMAT_R8}},
};

uint32_t GetGbmUseFlags(uint32_t hal_format, uint32_t hal_usage) {
  uint32_t gbm_flags = 0;
  if (hal_format != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED ||
      !(hal_usage & GRALLOC_USAGE_HW_CAMERA_READ)) {
    // The default GBM flags for non-private-reprocessing camera buffers.
    gbm_flags = GBM_BO_USE_SW_READ_OFTEN | GBM_BO_USE_SW_WRITE_OFTEN;
  }

  if (hal_usage & GRALLOC_USAGE_HW_CAMERA_READ) {
    gbm_flags |= GBM_BO_USE_CAMERA_READ;
  }
  if (hal_usage & GRALLOC_USAGE_HW_CAMERA_WRITE) {
    gbm_flags |= GBM_BO_USE_CAMERA_WRITE;
  }
  if (hal_usage & GRALLOC_USAGE_HW_TEXTURE) {
    gbm_flags |= GBM_BO_USE_TEXTURING;
  }
  if (hal_usage & GRALLOC_USAGE_HW_RENDER) {
    gbm_flags |= GBM_BO_USE_RENDERING;
  }
  if (hal_usage & GRALLOC_USAGE_HW_COMPOSER) {
    gbm_flags |= GBM_BO_USE_SCANOUT | GBM_BO_USE_TEXTURING;
  }
  if (hal_usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
    gbm_flags |= GBM_BO_USE_HW_VIDEO_ENCODER;
  }
  return gbm_flags;
}

bool IsMatchingFormat(int32_t hal_pixel_format, uint32_t drm_format) {
  switch (hal_pixel_format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
      return drm_format == DRM_FORMAT_ABGR8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
      return drm_format == DRM_FORMAT_XBGR8888;
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return drm_format == DRM_FORMAT_ARGB8888;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
      return drm_format == DRM_FORMAT_NV21;
    case HAL_PIXEL_FORMAT_YCbCr_422_I:
      return drm_format == DRM_FORMAT_ABGR8888;
    case HAL_PIXEL_FORMAT_BLOB:
      return drm_format == DRM_FORMAT_R8;
    case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
      // We can't really check implementation defined formats.
      return true;
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
      return (drm_format == DRM_FORMAT_YUV420 ||
              drm_format == DRM_FORMAT_YVU420 ||
              drm_format == DRM_FORMAT_NV21 || drm_format == DRM_FORMAT_NV12);
    case HAL_PIXEL_FORMAT_YV12:
      return drm_format == DRM_FORMAT_YVU420;
  }
  return false;
}

size_t GetChromaStep(uint32_t drm_format) {
  switch (drm_format) {
    case DRM_FORMAT_P010:
      return 4;
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
      return 2;
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
      return 1;
  }
  return 0;
}

uint8_t* GetPlaneAddr(const android_ycbcr& ycbcr,
                      uint32_t drm_format,
                      size_t plane) {
  void* result = nullptr;
  if (plane == 0) {
    result = ycbcr.y;
  } else if (plane == 1) {
    switch (drm_format) {
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_P010:
      case DRM_FORMAT_YUV420:
        result = ycbcr.cb;
        break;

      case DRM_FORMAT_NV21:
      case DRM_FORMAT_YVU420:
        result = ycbcr.cr;
        break;
    }
  } else if (plane == 2) {
    switch (drm_format) {
      case DRM_FORMAT_YUV420:
        result = ycbcr.cr;
        break;

      case DRM_FORMAT_YVU420:
        result = ycbcr.cb;
        break;
    }
  }
  if (result == nullptr) {
    LOGF(ERROR) << "Unsupported DRM pixel format: "
                << FormatToString(drm_format);
  }
  return reinterpret_cast<uint8_t*>(result);
}

}  // namespace

void BufferHandleDeleter::operator()(buffer_handle_t* handle) {
  if (handle) {
    auto* buf_mgr = cros::CameraBufferManager::GetInstance();
    if (buf_mgr && *handle != nullptr) {
      buf_mgr->Free(*handle);
    }
    delete handle;
  }
}

//
// ScopedMapping implementations.
//

ScopedMapping::ScopedMapping(buffer_handle_t buffer) : buf_(buffer) {
  for (size_t i = 0; i < num_planes(); ++i) {
    planes_[i] = {
        .stride = CameraBufferManager::GetPlaneStride(buf_, i),
        .size = CameraBufferManager::GetPlaneSize(buf_, i),
    };
  }
  auto* buf_mgr = CameraBufferManager::GetInstance();
  if (buf_mgr->Register(buf_) != 0) {
    LOGF(ERROR) << "Cannot register buffer";
    Invalidate();
    return;
  }
  if (num_planes() == 1) {
    void** addr_to_void = reinterpret_cast<void**>(&planes_[0].addr);
    if (buf_mgr->Lock(buf_, 0, 0, 0, width(), height(), addr_to_void) != 0) {
      LOGF(ERROR) << "Cannot Lock buffer";
      Invalidate();
      return;
    }
  } else {
    android_ycbcr ycbcr = {};
    if (buf_mgr->LockYCbCr(buf_, 0, 0, 0, width(), height(), &ycbcr) != 0) {
      LOGF(ERROR) << "Cannot Lock buffer";
      Invalidate();
      return;
    }
    for (size_t i = 0; i < num_planes(); ++i) {
      planes_[i].addr = GetPlaneAddr(ycbcr, drm_format(), i);
    }
  }
}

ScopedMapping::~ScopedMapping() {
  Invalidate();
}

ScopedMapping::ScopedMapping(ScopedMapping&& other) {
  *this = std::move(other);
}

ScopedMapping& ScopedMapping::operator=(ScopedMapping&& other) {
  if (this != &other) {
    Invalidate();
    planes_ = other.planes_;
    buf_ = other.buf_;
    other.planes_.fill(Plane());
    other.buf_ = nullptr;
  }
  return *this;
}

uint32_t ScopedMapping::width() const {
  return CameraBufferManager::GetWidth(buf_);
}

uint32_t ScopedMapping::height() const {
  return CameraBufferManager::GetHeight(buf_);
}

uint32_t ScopedMapping::drm_format() const {
  return CameraBufferManager::GetDrmPixelFormat(buf_);
}

uint32_t ScopedMapping::v4l2_format() const {
  return CameraBufferManager::GetV4L2PixelFormat(buf_);
}

uint32_t ScopedMapping::hal_pixel_format() const {
  return CameraBufferManager::GetHalPixelFormat(buf_);
}

uint32_t ScopedMapping::num_planes() const {
  return CameraBufferManager::GetNumPlanes(buf_);
}

ScopedMapping::Plane ScopedMapping::plane(int plane) const {
  if (plane > planes_.size()) {
    return Plane();
  }
  return planes_[plane];
}

bool ScopedMapping::is_valid() const {
  return buf_ != nullptr;
}

void ScopedMapping::Invalidate() {
  if (buf_ == nullptr) {
    return;
  }
  auto* buf_mgr = CameraBufferManager::GetInstance();
  buf_mgr->Unlock(buf_);
  buf_mgr->Deregister(buf_);
  planes_.fill(Plane());
  buf_ = nullptr;
}

//
// CameraBufferManagerImpl implementations.
//

// static
CameraBufferManager* CameraBufferManager::GetInstance() {
  static base::NoDestructor<CameraBufferManagerImpl> instance(
      cros::CreateMinigbmAllocator());
  if (!instance->allocator_) {
    LOGF(ERROR)
        << "Failed to create the buffer allocator for CameraBufferManager";
    return nullptr;
  }
  return instance.get();
}

// static
bool CameraBufferManager::IsValidBuffer(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return false;
  }
  if (!IsMatchingFormat(handle->hal_pixel_format, handle->drm_format)) {
    LOGF(ERROR) << "HAL pixel format " << handle->hal_pixel_format
                << " does not match DRM format "
                << FormatToString(handle->drm_format);
    return false;
  }
  return true;
}

// static
uint32_t CameraBufferManager::GetWidth(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }

  return handle->width;
}

// static
uint32_t CameraBufferManager::GetHeight(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }

  return handle->height;
}

// static
uint32_t CameraBufferManager::GetNumPlanes(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }

  switch (handle->drm_format) {
    case DRM_FORMAT_ABGR1555:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_ABGR4444:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_ARGB1555:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ARGB4444:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_AYUV:
    case DRM_FORMAT_BGR233:
    case DRM_FORMAT_BGR565:
    case DRM_FORMAT_BGR888:
    case DRM_FORMAT_BGRA1010102:
    case DRM_FORMAT_BGRA4444:
    case DRM_FORMAT_BGRA5551:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_BGRX1010102:
    case DRM_FORMAT_BGRX4444:
    case DRM_FORMAT_BGRX5551:
    case DRM_FORMAT_BGRX8888:
    case DRM_FORMAT_C8:
    case DRM_FORMAT_GR88:
    case DRM_FORMAT_R8:
    case DRM_FORMAT_RG88:
    case DRM_FORMAT_RGB332:
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_RGB888:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_RGBA4444:
    case DRM_FORMAT_RGBA5551:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_RGBX1010102:
    case DRM_FORMAT_RGBX4444:
    case DRM_FORMAT_RGBX5551:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_UYVY:
    case DRM_FORMAT_VYUY:
    case DRM_FORMAT_XBGR1555:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_XBGR4444:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_XRGB1555:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XRGB4444:
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_YVYU:
    case DRM_FORMAT_MTISP_SXYZW10:
      return 1;
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_P010:
      return 2;
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
      return 3;
  }

  LOGF(ERROR) << "Unknown format: " << FormatToString(handle->drm_format);
  return 0;
}

// static
uint32_t CameraBufferManager::GetV4L2PixelFormat(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }

  uint32_t num_planes = GetNumPlanes(buffer);
  if (!num_planes) {
    return 0;
  }

  bool is_mplane = false;
  if (num_planes > 1) {
    // Check if the buffer has multiple physical planes by checking the offsets
    // of each plane.  If any of the offsets is zero, then we assume the buffer
    // is of multi-planar format.
    for (size_t i = 1; i < num_planes; ++i) {
      if (!handle->offsets[i]) {
        is_mplane = true;
      }
    }
  }

  switch (handle->drm_format) {
    case DRM_FORMAT_ARGB8888:
      return V4L2_PIX_FMT_ABGR32;

    // There is no standard V4L2 pixel format corresponding to
    // DRM_FORMAT_xBGR8888.  We use our own V4L2 format extension
    // V4L2_PIX_FMT_RGBX32 here.
    case DRM_FORMAT_ABGR8888:
      return V4L2_PIX_FMT_RGBX32;
    case DRM_FORMAT_XBGR8888:
      return V4L2_PIX_FMT_RGBX32;

    // The format used by MediaTek ISP for private reprocessing. Note that the
    // V4L2 format used here is a default placeholder. The actual pixel format
    // varies depending on sensor settings.
    case DRM_FORMAT_MTISP_SXYZW10:
      return V4L2_PIX_FMT_MTISP_SBGGR10;

    // DRM_FORMAT_R8 is used as the underlying buffer format for
    // HAL_PIXEL_FORMAT_BLOB which corresponds to JPEG buffer.
    case DRM_FORMAT_R8:
      return V4L2_PIX_FMT_JPEG;

    // Semi-planar formats.
    case DRM_FORMAT_NV12:
      return is_mplane ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV12;
    case DRM_FORMAT_NV21:
      return is_mplane ? V4L2_PIX_FMT_NV21M : V4L2_PIX_FMT_NV21;
    case DRM_FORMAT_P010:
      return is_mplane ? V4L2_PIX_FMT_P010M : V4L2_PIX_FMT_P010;

    // Multi-planar formats.
    case DRM_FORMAT_YUV420:
      return is_mplane ? V4L2_PIX_FMT_YUV420M : V4L2_PIX_FMT_YUV420;
    case DRM_FORMAT_YVU420:
      return is_mplane ? V4L2_PIX_FMT_YVU420M : V4L2_PIX_FMT_YVU420;
  }

  LOGF(ERROR) << "Could not convert format "
              << FormatToString(handle->drm_format) << " to V4L2 pixel format";
  return 0;
}

// static
size_t CameraBufferManager::GetPlaneStride(buffer_handle_t buffer,
                                           size_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }
  if (plane >= GetNumPlanes(buffer)) {
    LOGF(ERROR) << "Invalid plane: " << plane;
    return 0;
  }
  return handle->strides[plane];
}

#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

// static
size_t CameraBufferManager::GetPlaneSize(buffer_handle_t buffer, size_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }
  if (plane >= GetNumPlanes(buffer)) {
    LOGF(ERROR) << "Invalid plane: " << plane;
    return 0;
  }
  uint32_t vertical_subsampling;
  switch (handle->drm_format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_P010:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
      vertical_subsampling = (plane == 0) ? 1 : 2;
      break;
    default:
      vertical_subsampling = 1;
  }
  return (handle->strides[plane] *
          DIV_ROUND_UP(handle->height, vertical_subsampling));
}

// static
off_t CameraBufferManager::GetPlaneOffset(buffer_handle_t buffer,
                                          size_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -1;
  }
  if (plane >= GetNumPlanes(buffer)) {
    LOGF(ERROR) << "Invalid plane: " << plane;
    return -1;
  }
  return handle->offsets[plane];
}

// static
uint64_t CameraBufferManager::GetModifier(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return DRM_FORMAT_MOD_INVALID;
  }

  return handle->modifier;
}

// static
int CameraBufferManager::GetPlaneFd(buffer_handle_t buffer, size_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -1;
  }
  if (plane >= GetNumPlanes(buffer)) {
    LOGF(ERROR) << "Invalid plane: " << plane;
    return -1;
  }
  return handle->fds[plane];
}

// static
uint32_t CameraBufferManager::GetHalPixelFormat(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }
  return handle->hal_pixel_format;
}

// static
uint32_t CameraBufferManager::GetDrmPixelFormat(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return 0;
  }
  return handle->drm_format;
}

CameraBufferManagerImpl::CameraBufferManagerImpl(
    std::unique_ptr<Allocator> gbm_allocator)
    : allocator_(std::move(gbm_allocator)) {}

int CameraBufferManagerImpl::Allocate(size_t width,
                                      size_t height,
                                      uint32_t hal_format,
                                      uint32_t hal_usage,
                                      buffer_handle_t* out_buffer,
                                      uint32_t* out_stride) {
  base::AutoLock l(lock_);

  uint32_t gbm_flags = 0;
  uint32_t drm_format = ResolveFormat(hal_format, hal_usage, &gbm_flags);
  if (!drm_format) {
    return -EINVAL;
  }

  BufferContext context = {
      .bo = allocator_->CreateBo(width, height, drm_format, gbm_flags)};
  if (!context.bo) {
    LOGF(ERROR) << "Failed to create GBM bo";
    return -ENOMEM;
  }

  auto handle = std::make_unique<camera_buffer_handle_t>();
  handle->base.version = sizeof(handle->base);
  handle->base.numInts = kCameraBufferHandleNumInts;
  handle->base.numFds = kCameraBufferHandleNumFds;
  handle->magic = kCameraBufferMagic;
  handle->buffer_id = context.bo->GetId();
  handle->drm_format = drm_format;
  handle->hal_pixel_format = hal_format;
  handle->hal_usage_flags = hal_usage;
  handle->width = width;
  handle->height = height;
  auto desc = context.bo->Describe();
  for (size_t i = 0; i < desc.num_planes; ++i) {
    handle->fds[i] = context.bo->GetPlaneFd(i);
    handle->strides[i] = desc.planes[i].row_stride;
    handle->offsets[i] = desc.planes[i].offset;
  }
  handle->modifier = desc.format_modifier;

  if (desc.num_planes == 1) {
    *out_stride = handle->strides[0];
  } else {
    *out_stride = 0;
  }
  *out_buffer = reinterpret_cast<buffer_handle_t>(handle.release());

  context.refcount = 1;
  buffers_[*out_buffer] = std::move(context);
  return 0;
}

// static
ScopedBufferHandle CameraBufferManager::AllocateScopedBuffer(
    size_t width, size_t height, uint32_t hal_format, uint32_t hal_usage) {
  auto* buf_mgr = CameraBufferManager::GetInstance();
  ScopedBufferHandle buffer(new buffer_handle_t(nullptr));
  uint32_t stride;
  if (buf_mgr->Allocate(width, height, hal_format, hal_usage, buffer.get(),
                        &stride) != 0) {
    LOGF(ERROR) << "Failed to allocate buffer";
    return nullptr;
  }
  DVLOGF(1) << "Buffer allocated -";
  DVLOGF(1) << "\tplanes: " << CameraBufferManager::GetNumPlanes(*buffer);
  DVLOGF(1) << "\twidth: " << CameraBufferManager::GetWidth(*buffer);
  DVLOGF(1) << "\theight: " << CameraBufferManager::GetHeight(*buffer);
  DVLOGF(1) << "\tformat: "
            << FormatToString(CameraBufferManager::GetDrmPixelFormat(*buffer));
  for (size_t i = 0; i < CameraBufferManager::GetNumPlanes(*buffer); ++i) {
    DVLOGF(1) << "\tplane" << i
              << " fd: " << CameraBufferManager::GetPlaneFd(*buffer, i);
    DVLOGF(1) << "\tplane" << i
              << " offset: " << CameraBufferManager::GetPlaneOffset(*buffer, i);
    DVLOGF(1) << "\tplane" << i
              << " stride: " << CameraBufferManager::GetPlaneStride(*buffer, i);
  }
  return buffer;
}

int CameraBufferManagerImpl::Free(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }

  Deregister(buffer);
  delete handle;
  return 0;
}

int CameraBufferManagerImpl::Register(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }

  base::AutoLock l(lock_);

  auto context_it = buffers_.find(buffer);
  if (context_it != buffers_.end()) {
    ++context_it->second.refcount;
    return 0;
  }

  // Import the buffer if we haven't done so.
  uint32_t num_planes = GetNumPlanes(buffer);
  if (num_planes == 0) {
    return -EINVAL;
  }
  ImportData data = {
      .desc = {
          .drm_format = handle->drm_format,
          .width = static_cast<int>(handle->width),
          .height = static_cast<int>(handle->height),
          .gbm_flags = GBM_BO_USE_CAMERA_READ | GBM_BO_USE_CAMERA_WRITE |
                       GBM_BO_USE_SW_READ_OFTEN | GBM_BO_USE_SW_WRITE_OFTEN,
          .num_planes = static_cast<int>(num_planes),
          .format_modifier = handle->modifier,
      }};
  for (size_t i = 0; i < num_planes; ++i) {
    // GBM doesn't need plane size and pixel_stride to import a BO.
    data.desc.planes[i] = {
        .offset = static_cast<int>(handle->offsets[i]),
        .row_stride = static_cast<int>(handle->strides[i]),
    };
    data.plane_fd[i] = handle->fds[i];
  }

  BufferContext context = {.bo = allocator_->ImportBo(data), .refcount = 1};
  if (!context.bo) {
    LOGF(ERROR) << "Failed to import buffer 0x" << std::hex
                << handle->buffer_id;
    return -EIO;
  }

  buffers_[buffer] = std::move(context);
  return 0;
}

int CameraBufferManagerImpl::Deregister(buffer_handle_t buffer) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }

  base::AutoLock l(lock_);

  auto context_it = buffers_.find(buffer);
  if (context_it == buffers_.end()) {
    LOGF(ERROR) << "Unknown buffer 0x" << std::hex << handle->buffer_id;
    return -EINVAL;
  }
  --context_it->second.refcount;
  if (context_it->second.refcount == 0) {
    buffers_.erase(context_it);
  }
  return 0;
}

int CameraBufferManagerImpl::Lock(buffer_handle_t buffer,
                                  uint32_t hal_usage,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t width,
                                  uint32_t height,
                                  void** out_addr) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }
  uint32_t num_planes = GetNumPlanes(buffer);
  if (!num_planes) {
    return -EINVAL;
  }
  if (num_planes > 1) {
    LOGF(ERROR) << "Lock called on multi-planar buffer 0x" << std::hex
                << handle->buffer_id;
    return -EINVAL;
  }
  if ((hal_usage & handle->hal_usage_flags) != hal_usage) {
    LOGF(ERROR) << "Incompatible usage flags: " << hal_usage
                << " (original usage flags: " << handle->hal_usage_flags << ")";
    return -EINVAL;
  }

  *out_addr = Map(buffer, hal_usage, 0);
  if (*out_addr == MAP_FAILED) {
    return -EINVAL;
  }
  return 0;
}

int CameraBufferManagerImpl::LockYCbCr(buffer_handle_t buffer,
                                       uint32_t hal_usage,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t width,
                                       uint32_t height,
                                       struct android_ycbcr* out_ycbcr) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }
  uint32_t num_planes = GetNumPlanes(buffer);
  if (!num_planes) {
    return -EINVAL;
  }
  if (num_planes < 2) {
    LOGF(ERROR) << "LockYCbCr called on single-planar buffer 0x" << std::hex
                << handle->buffer_id;
    return -EINVAL;
  }
  if ((hal_usage & handle->hal_usage_flags) != hal_usage) {
    LOGF(ERROR) << "Incompatible usage flags: " << hal_usage
                << " (original usage flags: " << handle->hal_usage_flags << ")";
    return -EINVAL;
  }

  DCHECK_LE(num_planes, 3u);
  std::vector<uint8_t*> addr(num_planes);
  for (size_t i = 0; i < num_planes; ++i) {
    void* a = Map(buffer, hal_usage, i);
    if (a == MAP_FAILED) {
      return -EINVAL;
    }
    addr[i] = reinterpret_cast<uint8_t*>(a);
  }
  out_ycbcr->y = addr[0];
  out_ycbcr->ystride = handle->strides[0];
  out_ycbcr->cstride = handle->strides[1];
  out_ycbcr->chroma_step = GetChromaStep(handle->drm_format);
  CHECK_GT(out_ycbcr->chroma_step, 0);

  if (num_planes == 2) {
    switch (handle->drm_format) {
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_P010:
        out_ycbcr->cb = addr[1];
        out_ycbcr->cr = addr[1] + (out_ycbcr->chroma_step / 2);
        break;

      case DRM_FORMAT_NV21:
        out_ycbcr->cb = addr[1] + (out_ycbcr->chroma_step / 2);
        out_ycbcr->cr = addr[1];
        break;

      default:
        LOGF(ERROR) << "Unsupported semi-planar format: "
                    << FormatToString(handle->drm_format);
        return -EINVAL;
    }
  } else {  // num_planes == 3
    switch (handle->drm_format) {
      case DRM_FORMAT_YUV420:
        out_ycbcr->cb = addr[1];
        out_ycbcr->cr = addr[2];
        break;

      case DRM_FORMAT_YVU420:
        out_ycbcr->cb = addr[2];
        out_ycbcr->cr = addr[1];
        break;

      default:
        LOGF(ERROR) << "Unsupported planar format: "
                    << FormatToString(handle->drm_format);
        return -EINVAL;
    }
  }
  return 0;
}

int CameraBufferManagerImpl::Unlock(buffer_handle_t buffer) {
  for (size_t i = 0; i < GetNumPlanes(buffer); ++i) {
    int ret = Unmap(buffer, i);
    if (ret) {
      return ret;
    }
  }
  return 0;
}

uint32_t CameraBufferManagerImpl::ResolveDrmFormat(uint32_t hal_format,
                                                   uint32_t hal_usage) {
  uint32_t unused_gbm_flags;
  return ResolveFormat(hal_format, hal_usage, &unused_gbm_flags);
}

uint32_t CameraBufferManagerImpl::ResolveFormat(uint32_t hal_format,
                                                uint32_t hal_usage,
                                                uint32_t* gbm_flags) {
  uint32_t gbm_usage = GetGbmUseFlags(hal_format, hal_usage);
  uint32_t drm_format = 0;
  if (hal_usage & GRALLOC_USAGE_FORCE_I420) {
    CHECK_EQ(hal_format, HAL_PIXEL_FORMAT_YCbCr_420_888);
    *gbm_flags = gbm_usage;
    return DRM_FORMAT_YUV420;
  }

  if (hal_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
      (hal_usage & GRALLOC_USAGE_HW_CAMERA_READ)) {
    // Check which private format the graphics backend support.
    if (allocator_->IsFormatSupported(DRM_FORMAT_MTISP_SXYZW10, gbm_usage)) {
      *gbm_flags = gbm_usage;
      return DRM_FORMAT_MTISP_SXYZW10;
    }
    // TODO(lnishan): Check other private formats when we have private formats
    // from other platforms.
  }

  if (kSupportedHalFormats.find(hal_format) == kSupportedHalFormats.end()) {
    LOGF(ERROR) << "Unsupported HAL pixel format";
    return 0;
  }

  for (uint32_t format : kSupportedHalFormats[hal_format]) {
    if (allocator_->IsFormatSupported(format, gbm_usage)) {
      drm_format = format;
      break;
    }
  }

  if (drm_format == 0 && hal_usage & GRALLOC_USAGE_HW_COMPOSER) {
    gbm_usage &= ~GBM_BO_USE_SCANOUT;
    for (uint32_t format : kSupportedHalFormats[hal_format]) {
      if (allocator_->IsFormatSupported(format, gbm_usage)) {
        drm_format = format;
        break;
      }
    }
  }

  if (drm_format == 0) {
    LOGF(ERROR) << "Cannot resolve the actual format of HAL pixel format "
                << hal_format;
    return 0;
  }

  *gbm_flags = gbm_usage;
  return drm_format;
}

void* CameraBufferManagerImpl::Map(buffer_handle_t buffer,
                                   uint32_t hal_usage,
                                   uint32_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return MAP_FAILED;
  }

  uint32_t num_planes = GetNumPlanes(buffer);
  if (!num_planes) {
    return MAP_FAILED;
  }
  if (!(plane < kMaxPlanes && plane < num_planes)) {
    LOGF(ERROR) << "Invalid plane: " << plane;
    return MAP_FAILED;
  }

  DVLOGF(2) << "buffer info:";
  DVLOGF(2) << "\tfd: " << handle->fds[plane];
  DVLOGF(2) << "\tbuffer_id: 0x" << std::hex << handle->buffer_id;
  DVLOGF(2) << "\tformat: " << FormatToString(handle->drm_format);
  DVLOGF(2) << "\twidth: " << handle->width;
  DVLOGF(2) << "\theight: " << handle->height;
  DVLOGF(2) << "\tstride: " << handle->strides[plane];
  DVLOGF(2) << "\toffset: " << handle->offsets[plane];

  base::AutoLock l(lock_);

  auto context_it = buffers_.find(buffer);
  if (context_it == buffers_.end()) {
    LOGF(ERROR) << "Buffer 0x" << std::hex << handle->buffer_id
                << " is not registered";
    return MAP_FAILED;
  }

  // Always map the whole buffer.
  auto& bo = context_it->second.bo;
  if (!bo->BeginCpuAccess(SyncType::kSyncReadWrite, plane)) {
    LOGF(ERROR) << "Failed to sync buffer for CPU access";
    return MAP_FAILED;
  }
  if (!bo->Map(plane)) {
    LOGF(ERROR) << "Failed to map buffer";
    return MAP_FAILED;
  }
  void* addr = bo->GetPlaneAddr(plane);
  DVLOGF(2) << "Plane " << plane << " of DMA-buf 0x" << std::hex
            << handle->buffer_id << " mapped to " << addr;
  return addr;
}

int CameraBufferManagerImpl::Unmap(buffer_handle_t buffer, uint32_t plane) {
  auto handle = camera_buffer_handle_t::FromBufferHandle(buffer);
  if (!handle) {
    return -EINVAL;
  }

  base::AutoLock l(lock_);

  auto context_it = buffers_.find(buffer);
  if (context_it == buffers_.end()) {
    LOGF(ERROR) << "Buffer 0x" << std::hex << handle->buffer_id
                << " is not registered";
    return -EINVAL;
  }

  auto& bo = context_it->second.bo;
  bo->Unmap(plane);
  if (!bo->EndCpuAccess(SyncType::kSyncReadWrite, plane)) {
    LOGF(ERROR) << "Failed to sync buffer after CPU access";
    return -EINVAL;
  }
  return 0;
}

}  // namespace cros
