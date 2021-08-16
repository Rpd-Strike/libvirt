fc_driver_sources = [
  'fc_conf.c',
  'fc_conf.h',
  'fc_driver.c',
  'fc_driver.h',
  'fc_domain.c',
  'fc_domain.h',
  'fc_monitor.c',
  'fc_monitor.h',
]

driver_source_files += files(fc_driver_sources)

stateful_driver_source_files += files(fc_driver_sources)

if conf.has('WITH_FC')
  fc_driver_impl = static_library(
    'virt_driver_fc_impl',
    [
      fc_driver_sources,
    ],
    dependencies: [
      access_dep,
      curl_dep,
      log_dep,
      src_dep,
    ],
    include_directories: [
      conf_inc_dir,
    ],
  )

  virt_modules += {
    'name': 'virt_driver_fc',
    'link_whole': [
      fc_driver_impl,
    ],
  }

  virt_daemons += {
    'name': 'virtfcd',
    'c_args': [
      '-DDAEMON_NAME="virtfcd"',
      '-DMODULE_NAME="fc"',
    ],
  }

  virt_daemon_confs += {
    'name': 'virtfcd',
  }

  virt_daemon_units += {
    'service': 'virtfcd',
    'service_in': files('virtfcd.service.in'),
    'name': 'Libvirt fc',
    'sockprefix': 'virtfcd',
    'sockets': [ 'main', 'ro', 'admin' ],
  }

  sysconf_files += {
    'name': 'virtfcd',
    'file': files('virtfcd.sysconf'),
  }

  virt_install_dirs += [
    localstatedir / 'lib' / 'libvirt' / 'fc',
    runstatedir / 'libvirt' / 'fc',
  ]
endif
