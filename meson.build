project('battery-stats', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3',
                     'cpp_std=c++23'])

exe = executable('battery-stats', 'battery_stats.cpp',
  dependencies: dependency('sdbusplus'),
  install : true)
