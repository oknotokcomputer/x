// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_perception/proto_mojom_conversion.h"

#include <gtest/gtest.h>
#include <string>
#include <utility>

const char kMockErrorSource[] = "Mock Error Source";
const char kMockErrorString[] = "Mock Error String";


namespace {

constexpr int kNumSupportedConfigurations = 3;

}  // namespace

namespace chromeos {
namespace media_perception {
namespace mojom {

namespace {


mri::VideoStreamParams CreateVideoStreamParamsProto(
    int width_in_pixels, int height_in_pixels,
    float frame_rate_in_frames_per_second) {
  mri::VideoStreamParams params;
  params.set_width_in_pixels(width_in_pixels);
  params.set_height_in_pixels(height_in_pixels);
  params.set_frame_rate_in_frames_per_second(
      frame_rate_in_frames_per_second);
  params.set_pixel_format(mri::PixelFormat::I420);
  return params;
}

mri::VideoDevice CreateVideoDeviceProto(
    std::string id, std::string display_name,
    std::string model_id, bool in_use) {
  mri::VideoDevice device;
  device.set_id(id);
  device.set_display_name(display_name);
  device.set_model_id(model_id);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    int j = i * kNumSupportedConfigurations;
    mri::VideoStreamParams* params = device.add_supported_configurations();
    *params = CreateVideoStreamParamsProto(j, j + 1, j + 2);
  }
  device.set_in_use(in_use);
  if (in_use) {
    mri::VideoStreamParams* params = device.mutable_configuration();
    *params = CreateVideoStreamParamsProto(1, 2, 3);
  }
  return device;
}

mri::AudioStreamParams CreateAudioStreamParamsProto(
    float frequency_in_hz, int num_channels) {
  mri::AudioStreamParams params;
  params.set_frequency_in_hz(frequency_in_hz);
  params.set_num_channels(num_channels);
  return params;
}

mri::AudioDevice CreateAudioDeviceProto(
    std::string id, std::string display_name) {
  mri::AudioDevice device;
  device.set_id(id);
  device.set_display_name(display_name);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    int j = i * kNumSupportedConfigurations;
    mri::AudioStreamParams* params = device.add_supported_configurations();
    *params = CreateAudioStreamParamsProto(j, j + 1);
  }
  mri::AudioStreamParams* params = device.mutable_configuration();
  *params = CreateAudioStreamParamsProto(1, 2);
  return device;
}

TEST(ProtoMojomConversionTest, VideoStreamParamsToMojom) {
  mri::VideoStreamParams params = CreateVideoStreamParamsProto(1, 2, 3);

  VideoStreamParamsPtr params_ptr = ToMojom(params);
  EXPECT_EQ(params_ptr->width_in_pixels, 1);
  EXPECT_EQ(params_ptr->height_in_pixels, 2);
  EXPECT_EQ(params_ptr->frame_rate_in_frames_per_second, 3);
  EXPECT_EQ(params_ptr->pixel_format, PixelFormat::I420);
}

TEST(ProtoMojomConversionTest, VideoDeviceToMojom) {
  mri::VideoDevice device = CreateVideoDeviceProto(
      "id", "display_name", "model_id", true);

  VideoDevicePtr device_ptr = ToMojom(device);
  EXPECT_EQ(device_ptr->id, "id");
  EXPECT_EQ(*device_ptr->display_name, "display_name");
  EXPECT_EQ(*device_ptr->model_id, "model_id");
  EXPECT_EQ(device_ptr->in_use, true);
  EXPECT_EQ(device_ptr->configuration->width_in_pixels, 1);
  EXPECT_EQ(device_ptr->configuration->height_in_pixels, 2);
  EXPECT_EQ(device_ptr->configuration->frame_rate_in_frames_per_second, 3);
  EXPECT_EQ(device_ptr->configuration->pixel_format, PixelFormat::I420);
  EXPECT_EQ(device_ptr->supported_configurations.size(),
            kNumSupportedConfigurations);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    EXPECT_EQ(device_ptr->supported_configurations[i]->width_in_pixels,
              i * kNumSupportedConfigurations);
  }
}

TEST(ProtoMojomConversionTest, VirtualVideoDeviceToMojom) {
  mri::VirtualVideoDevice device;
  mri::VideoDevice* video_device = device.mutable_video_device();
  *video_device = CreateVideoDeviceProto(
      "id", "display_name", "model_id", true);
  VirtualVideoDevicePtr device_ptr = ToMojom(device);
  EXPECT_EQ(device_ptr->video_device->id, "id");
}

TEST(ProtoMojomConversionTest, AudioStreamParamsToMojom) {
  mri::AudioStreamParams params = CreateAudioStreamParamsProto(
      1, 2);
  AudioStreamParamsPtr params_ptr = ToMojom(params);
  EXPECT_EQ(params_ptr->frequency_in_hz, 1);
  EXPECT_EQ(params_ptr->num_channels, 2);
}

TEST(ProtoMojomConversionTest, AudioDeviceToMojom) {
  mri::AudioDevice device = CreateAudioDeviceProto(
      "id", "display_name");
  AudioDevicePtr device_ptr = ToMojom(device);
  EXPECT_EQ(device_ptr->id, "id");
  EXPECT_EQ(*device_ptr->display_name, "display_name");
  EXPECT_EQ(device_ptr->configuration->frequency_in_hz, 1);
  EXPECT_EQ(device_ptr->configuration->num_channels, 2);
  EXPECT_EQ(device_ptr->supported_configurations.size(),
            kNumSupportedConfigurations);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    EXPECT_EQ(device_ptr->supported_configurations[i]->frequency_in_hz,
              i * kNumSupportedConfigurations);
  }
}

TEST(ProtoMojomConversionTest, DeviceTemplateToMojom) {
  mri::DeviceTemplate device_template;
  device_template.set_template_name("template_name");
  device_template.set_device_type(mri::DeviceType::VIRTUAL_VIDEO);
  DeviceTemplatePtr template_ptr = ToMojom(device_template);
  EXPECT_EQ(template_ptr->template_name, "template_name");
  EXPECT_EQ(template_ptr->device_type, DeviceType::VIRTUAL_VIDEO);
}

TEST(ProtoMojomConversionTest, NormalizedBoundingBoxToMojom) {
  mri::NormalizedBoundingBox bbox;
  bbox.set_x_min(0.1);
  bbox.set_y_min(0.2);
  bbox.set_x_max(0.7);
  bbox.set_y_max(0.8);
  bbox.set_normalization_width(10);
  bbox.set_normalization_height(20);

  NormalizedBoundingBoxPtr bbox_ptr = ToMojom(bbox);
  EXPECT_FLOAT_EQ(bbox_ptr->x_min, 0.1);
  EXPECT_FLOAT_EQ(bbox_ptr->y_min, 0.2);
  EXPECT_FLOAT_EQ(bbox_ptr->x_max, 0.7);
  EXPECT_FLOAT_EQ(bbox_ptr->y_max, 0.8);
  EXPECT_EQ(bbox_ptr->normalization_width, 10);
  EXPECT_EQ(bbox_ptr->normalization_height, 20);
}

TEST(ProtoMojomConversionTest, DistanceToMojom) {
  mri::Distance distance;
  distance.set_units(mri::DistanceUnits::METERS);
  distance.set_magnitude(1.5);

  DistancePtr distance_ptr = ToMojom(distance);
  EXPECT_EQ(distance_ptr->units, DistanceUnits::METERS);
  EXPECT_FLOAT_EQ(distance_ptr->magnitude, 1.5);
}

TEST(ProtoMojomConversionTest, PipelineErrorToMojom) {
  mri::PipelineError error;
  error.set_error_type(mri::PipelineErrorType::CONFIGURATION);
  error.set_error_source(kMockErrorSource);
  error.set_error_string(kMockErrorString);

  PipelineErrorPtr error_ptr = ToMojom(error);
  EXPECT_EQ(error_ptr->error_type, PipelineErrorType::CONFIGURATION);
  EXPECT_EQ(*error_ptr->error_source, kMockErrorSource);
  EXPECT_EQ(*error_ptr->error_string, kMockErrorString);
}

TEST(ProtoMojomConversionTest, PipelineStateToMojom) {
  mri::PipelineState state;
  state.set_status(mri::PipelineStatus::RUNNING);

  mri::PipelineError& error = *state.mutable_error();
  error.set_error_type(mri::PipelineErrorType::CONFIGURATION);
  error.set_error_source(kMockErrorSource);
  error.set_error_string(kMockErrorString);

  PipelineStatePtr state_ptr = ToMojom(state);
  EXPECT_EQ(state_ptr->status, PipelineStatus::RUNNING);

  PipelineErrorPtr& error_ptr = state_ptr->error;
  EXPECT_EQ(error_ptr->error_type, PipelineErrorType::CONFIGURATION);
  EXPECT_EQ(*error_ptr->error_source, kMockErrorSource);
  EXPECT_EQ(*error_ptr->error_string, kMockErrorString);
}

}  // namespace
}  // namespace mojom
}  // namespace media_perception
}  // namespace chromeos

namespace mri {
namespace {

chromeos::media_perception::mojom::VideoStreamParamsPtr
CreateVideoStreamParamsPtr(
    int width_in_pixels, int height_in_pixels,
    float frame_rate_in_frames_per_second) {
  chromeos::media_perception::mojom::VideoStreamParamsPtr params_ptr =
      chromeos::media_perception::mojom::VideoStreamParams::New();
  params_ptr->width_in_pixels = width_in_pixels;
  params_ptr->height_in_pixels = height_in_pixels;
  params_ptr->frame_rate_in_frames_per_second = frame_rate_in_frames_per_second;
  params_ptr->pixel_format =
      chromeos::media_perception::mojom::PixelFormat::I420;
  return params_ptr;
}

chromeos::media_perception::mojom::VideoDevicePtr CreateVideoDevicePtr(
    std::string id, std::string display_name,
    std::string model_id, bool in_use) {
  chromeos::media_perception::mojom::VideoDevicePtr device_ptr =
      chromeos::media_perception::mojom::VideoDevice::New();
  device_ptr->id = id;
  device_ptr->display_name = display_name;
  device_ptr->model_id = model_id;
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    int j = i * kNumSupportedConfigurations;
    device_ptr->supported_configurations.push_back(
        CreateVideoStreamParamsPtr(j, j + 1, j + 2));
  }
  device_ptr->in_use = in_use;
  if (in_use) {
    device_ptr->configuration = CreateVideoStreamParamsPtr(
        1, 2, 3);
  }
  return device_ptr;
}

chromeos::media_perception::mojom::AudioStreamParamsPtr
CreateAudioStreamParamsPtr(
    float frequency_in_hz, int num_channels) {
  chromeos::media_perception::mojom::AudioStreamParamsPtr params_ptr =
      chromeos::media_perception::mojom::AudioStreamParams::New();
  params_ptr->frequency_in_hz = frequency_in_hz;
  params_ptr->num_channels = num_channels;
  return params_ptr;
}

chromeos::media_perception::mojom::AudioDevicePtr CreateAudioDevicePtr(
    std::string id, std::string display_name) {
  chromeos::media_perception::mojom::AudioDevicePtr device_ptr =
      chromeos::media_perception::mojom::AudioDevice::New();
  device_ptr->id = id;
  device_ptr->display_name = display_name;
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    int j = i * kNumSupportedConfigurations;
    device_ptr->supported_configurations.push_back(
        CreateAudioStreamParamsPtr(j, j + 1));
  }
  device_ptr->configuration = CreateAudioStreamParamsPtr(1, 2);
  return device_ptr;
}

TEST(ProtoMojomConversionTest, VideoStreamParamsToProto) {
  chromeos::media_perception::mojom::VideoStreamParamsPtr params_ptr;
  VideoStreamParams params = ToProto(params_ptr);
  EXPECT_EQ(params.width_in_pixels(), 0);

  params = ToProto(CreateVideoStreamParamsPtr(1, 2, 3));
  EXPECT_EQ(params.width_in_pixels(), 1);
  EXPECT_EQ(params.height_in_pixels(), 2);
  EXPECT_EQ(params.frame_rate_in_frames_per_second(), 3);
  EXPECT_EQ(params.pixel_format(), PixelFormat::I420);
}

TEST(ProtoMojomConversionTest, VideoDeviceToProto) {
  chromeos::media_perception::mojom::VideoDevicePtr device_ptr =
      CreateVideoDevicePtr(
          "id", "display_name", "model_id", true);

  VideoDevice device = ToProto(device_ptr);
  EXPECT_EQ(device.id(), "id");
  EXPECT_EQ(device.display_name(), "display_name");
  EXPECT_EQ(device.model_id(), "model_id");
  EXPECT_EQ(device.in_use(), true);
  EXPECT_EQ(device.configuration().width_in_pixels(), 1);
  EXPECT_EQ(device.configuration().height_in_pixels(), 2);
  EXPECT_EQ(device.configuration().frame_rate_in_frames_per_second(), 3);
  EXPECT_EQ(device.configuration().pixel_format(), PixelFormat::I420);
  EXPECT_EQ(device.supported_configurations().size(),
            kNumSupportedConfigurations);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    EXPECT_EQ(device.supported_configurations(i).width_in_pixels(),
              i * kNumSupportedConfigurations);
  }
}

TEST(ProtoMojomConversionTest, VirtualVideoDeviceToProto) {
  chromeos::media_perception::mojom::VirtualVideoDevicePtr device_ptr =
      chromeos::media_perception::mojom::VirtualVideoDevice::New();
  device_ptr->video_device = CreateVideoDevicePtr(
      "id", "display_name", "model_id", true);
  VirtualVideoDevice device = ToProto(device_ptr);
  EXPECT_EQ(device.video_device().id(), "id");
}

TEST(ProtoMojomConversionTest, AudioStreamParamsToProto) {
  chromeos::media_perception::mojom::AudioStreamParamsPtr params_ptr;
  AudioStreamParams params = ToProto(params_ptr);
  EXPECT_EQ(params.frequency_in_hz(), 0);

  params = ToProto(CreateAudioStreamParamsPtr(1, 2));
  EXPECT_EQ(params.frequency_in_hz(), 1);
  EXPECT_EQ(params.num_channels(), 2);
}

TEST(ProtoMojomConversionTest, AudioDeviceToProto) {
  chromeos::media_perception::mojom::AudioDevicePtr device_ptr =
      CreateAudioDevicePtr("id", "display_name");

  AudioDevice device = ToProto(device_ptr);
  EXPECT_EQ(device.id(), "id");
  EXPECT_EQ(device.display_name(), "display_name");
  EXPECT_EQ(device.configuration().frequency_in_hz(), 1);
  EXPECT_EQ(device.configuration().num_channels(), 2);
  EXPECT_EQ(device.supported_configurations().size(),
            kNumSupportedConfigurations);
  for (int i = 0; i < kNumSupportedConfigurations; i++) {
    EXPECT_EQ(device.supported_configurations(i).frequency_in_hz(),
              i * kNumSupportedConfigurations);
  }
}

TEST(ProtoMojomConversionTest, DeviceTemplateToProto) {
  chromeos::media_perception::mojom::DeviceTemplatePtr template_ptr =
      chromeos::media_perception::mojom::DeviceTemplate::New();
  template_ptr->template_name = "template_name";
  template_ptr->device_type =
      chromeos::media_perception::mojom::DeviceType::VIRTUAL_VIDEO;
  DeviceTemplate device_template = ToProto(template_ptr);
  EXPECT_EQ(device_template.template_name(), "template_name");
  EXPECT_EQ(device_template.device_type(), DeviceType::VIRTUAL_VIDEO);
}

TEST(ProtoMojomConversionTest, NormalizedBoundingBoxToProto) {
  chromeos::media_perception::mojom::NormalizedBoundingBoxPtr bbox_ptr =
      chromeos::media_perception::mojom::NormalizedBoundingBox::New();
  bbox_ptr->x_min = 0.1;
  bbox_ptr->y_min = 0.2;
  bbox_ptr->x_max = 0.7;
  bbox_ptr->y_max = 0.8;
  bbox_ptr->normalization_width = 10;
  bbox_ptr->normalization_height = 20;

  NormalizedBoundingBox bbox = ToProto(bbox_ptr);
  EXPECT_FLOAT_EQ(bbox.x_min(), 0.1);
  EXPECT_FLOAT_EQ(bbox.y_min(), 0.2);
  EXPECT_FLOAT_EQ(bbox.x_max(), 0.7);
  EXPECT_FLOAT_EQ(bbox.y_max(), 0.8);
  EXPECT_FLOAT_EQ(bbox.normalization_width(), 10);
  EXPECT_FLOAT_EQ(bbox.normalization_height(), 20);
}

TEST(ProtoMojomConversionTest, DistanceToProto) {
  chromeos::media_perception::mojom::DistancePtr distance_ptr =
      chromeos::media_perception::mojom::Distance::New();
  distance_ptr->units =
      chromeos::media_perception::mojom::DistanceUnits::METERS;
  distance_ptr->magnitude = 1.5;

  Distance distance = ToProto(distance_ptr);
  EXPECT_EQ(distance.units(), DistanceUnits::METERS);
  EXPECT_FLOAT_EQ(distance.magnitude(), 1.5);
}

TEST(ProtoMojomConversionTest, PipelineErrorToProto) {
  // Construct mojom ptr for PipelineError.
  chromeos::media_perception::mojom::PipelineErrorPtr error_ptr =
    chromeos::media_perception::mojom::PipelineError::New();
  error_ptr->error_type =
    chromeos::media_perception::mojom::PipelineErrorType::CONFIGURATION;
  *error_ptr->error_source = kMockErrorSource;
  *error_ptr->error_string = kMockErrorString;

  PipelineError error = ToProto(error_ptr);
  EXPECT_EQ(error.error_type(), PipelineErrorType::CONFIGURATION);
  EXPECT_EQ(error.error_source(), kMockErrorSource);
  EXPECT_EQ(error.error_string(), kMockErrorString);
}

TEST(ProtoMojomConversionTest, PipelineStateToProto) {
  chromeos::media_perception::mojom::PipelineStatePtr state_ptr =
    chromeos::media_perception::mojom::PipelineState::New();
  state_ptr->status =
    chromeos::media_perception::mojom::PipelineStatus::RUNNING;

  state_ptr->error = chromeos::media_perception::mojom::PipelineError::New();

  state_ptr->error->error_type =
    chromeos::media_perception::mojom::PipelineErrorType::CONFIGURATION;
  *state_ptr->error->error_source = kMockErrorSource;
  *state_ptr->error->error_string = kMockErrorString;

  PipelineState state = ToProto(state_ptr);
  EXPECT_EQ(state.status(), PipelineStatus::RUNNING);
  EXPECT_EQ(state.error().error_type(), PipelineErrorType::CONFIGURATION);
  EXPECT_EQ(state.error().error_source(), kMockErrorSource);
  EXPECT_EQ(state.error().error_string(), kMockErrorString);
}

}  // namespace
}  // namespace mri
