{
  'target_defaults': {
    'variables': {
      'deps': [
        'libchrome-<(libbase_ver)',
      ],
    },
  },

  'targets': [
    {
      'target_name': 'touchkb_haptic_test',
      'type': 'executable',
      'variables': {
        'deps': [
          'libbrillo-<(libbase_ver)',
        ],
      },
      'sources': [
        'haptic/haptic_test.cc',
        'haptic/ff_driver.cc',
      ],
    },
  ],
}
