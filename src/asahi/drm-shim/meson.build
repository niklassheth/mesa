# Copyright 2019 Broadcom
# SPDX-License-Identifier: MIT

dep_python_embed = dependency('python3-embed')

libasahi_noop_drm_shim = shared_library(
  'asahi_noop_drm_shim',
  'asahi_m1n1.c',
  include_directories: [inc_include, inc_src],
  dependencies: [dep_drm_shim, dep_valgrind, dep_python_embed],
  gnu_symbol_visibility : 'hidden',
  install : true,
)
