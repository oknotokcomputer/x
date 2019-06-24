{
  'includes': [
    '../build/cros-camera-common.gypi',
  ],
  'target_defaults': {
    'variables': {
      'deps': [
        'cros-camera-android-headers',
        'gbm',
        'libcamera_client',
        'libcamera_common',
        'libcamera_metadata',
        'libcbm',
        'libdrm',
        'libexif',
        'libsync',
        'libyuv',
      ],
    },
  },
  'targets': [
    {
      'target_name': 'cros_camera_test',
      'type': 'executable',
      'includes': [
        '../../common-mk/common_test.gypi',
      ],
      'libraries': [
        '-ldl',
        '-ljpeg',
      ],
      'sources': [
        '../common/utils/camera_hal_enumerator.cc',
        'camera3_device_impl.cc',
        'camera3_device_test.cc',
        'camera3_exif_validator.cc',
        'camera3_frame_test.cc',
        'camera3_module_test.cc',
        'camera3_perf_log.cc',
        'camera3_preview_test.cc',
        'camera3_recording_test.cc',
        'camera3_reprocessing_test.cc',
        'camera3_service.cc',
        'camera3_still_capture_test.cc',
        'camera3_stream_test.cc',
        'camera3_test_data_forwarder.cc',
        'camera3_test_gralloc.cc',
      ],
    },
  ],
  'conditions': [
    ['USE_fuzzer == 1', {
      'targets': [
        {
          'target_name': 'cros_camera_fuzzer',
          'type': 'executable',
          'includes': [
            '../../common-mk/common_fuzzer.gypi',
            '../../common-mk/common_test.gypi',
          ],
          'defines': [
            'FUZZER',
          ],
          'cflags': [
            '-g',
          ],
          'libraries': [
            '-ldl',
            '-ljpeg',
          ],
          'sources': [
            '../common/utils/camera_hal_enumerator.cc',
            'camera3_device_impl.cc',
            'camera3_device_test.cc',
            'camera3_exif_validator.cc',
            'camera3_frame_test.cc',
            'camera3_module_test.cc',
            'camera3_perf_log.cc',
            'camera3_preview_test.cc',
            'camera3_recording_test.cc',
            'camera3_reprocessing_test.cc',
            'camera3_service.cc',
            'camera3_still_capture_test.cc',
            'camera3_stream_test.cc',
            'camera3_test_data_forwarder.cc',
            'camera3_test_gralloc.cc',
          ],
        },
      ],
    }],
  ],
}
