{
  'target_defaults': {
    'variables': {
      'deps': [
        'libbrillo-<(libbase_ver)',
        'libchrome-<(libbase_ver)',
        'libmetrics-<(libbase_ver)',
        # system_api depends on protobuf (or protobuf-lite). It must appear
        # before protobuf here or the linker flags won't be in the right
        # order.
        'system_api',
        'protobuf-lite',
      ],
    },
  },
  'targets': [
    {
      'target_name': 'libbiod',
      'type': 'static_library',
      'sources': [
        'biod_metrics.cc',
        'biod_storage.cc',
        'biometrics_daemon.cc',
        'cros_fp_device.cc',
        'cros_fp_biometrics_manager.cc',
        'scoped_umask.cc',
        'uinput_device.cc',
      ],
    },
    {
      'target_name': 'biod',
      'type': 'executable',
      'variables': {
        'USE_arm%': 0,
        'USE_arm64%': 0,
      },
      'link_settings': {
        'libraries': [
          '-ldl',
        ],
      },
      'dependencies': ['libbiod'],
      'sources': [
        'main.cc',
      ],
    },
    {
      'target_name': 'biod_client_tool',
      'type': 'executable',
      'sources': ['tools/biod_client_tool.cc'],
    },
    {
      'target_name': 'bio_crypto_init',
      'type': 'executable',
      'dependencies': ['libbiod'],
      'sources': ['tools/bio_crypto_init.cc'],
    },
    {
      'target_name': 'bio_wash',
      'type': 'executable',
      'dependencies': ['libbiod'],
      'sources': ['tools/bio_wash.cc'],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'biod_test_runner',
          'type': 'executable',
          'dependencies': [
            'libbiod',
            '../common-mk/testrunner.gyp:testrunner',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'variables': {
            'deps': [
              'libchrome-test-<(libbase_ver)',
            ],
          },
          'sources': [
            'biod_metrics_test.cc',
            'biod_storage_test.cc',
          ],
        },
      ],
    }],
  ],
}
