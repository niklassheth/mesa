# Copyright 2017 Intel Corporation
# SPDX-License-Identifier: MIT

files_asahi = files(
  'driinfo_asahi.h',
  'agx_batch.c',
  'agx_apple9.c',
  'agx_blit.c',
  'agx_disk_cache.c',
  'agx_fence.c',
  'agx_pipe.c',
  'agx_nir_lower_sysvals.c',
  'agx_nir_lower_bindings.c',
  'agx_nir_lower_point_size.c',
  'agx_query.c',
  'agx_state.c',
  'agx_streamout.c',
  'agx_uniforms.c',
)

libasahi = static_library(
  'asahi',
  files_asahi,
  include_directories : [inc_gallium_aux, inc_gallium, inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
  c_args : [c_msvc_compat_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_nir, idep_mesautil, idep_agx_pack, dep_libdrm, idep_libagx, idep_libagx_shaders_h],
)

driver_asahi = declare_dependency(
  compile_args : '-DGALLIUM_ASAHI',
  link_with : [libasahi, libasahiwinsys, libasahi_compiler, libasahi_lib, libasahi_layout, libasahi_decode]
)

if with_tools.contains('drm-shim')
  t8132_gallium_compute_xyz = executable(
    't8132_gallium_compute_xyz',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_xyz.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  t8132_gallium_compute_xy = executable(
    't8132_gallium_compute_xy',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_xy.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  t8132_gallium_compute_dag3 = executable(
    't8132_gallium_compute_dag3',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_dag3.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, idep_libagx,
                   idep_libagx_shaders_h, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  t8132_gallium_compute_device_index = executable(
    't8132_gallium_compute_device_index',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_device_index.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, idep_libagx,
                   idep_libagx_shaders_h, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  t8132_gallium_compute_device_graph = executable(
    't8132_gallium_compute_device_graph',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_device_graph.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, idep_libagx,
                   idep_libagx_shaders_h, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  t8132_gallium_compute_mix4 = executable(
    't8132_gallium_compute_mix4',
    [
      '../../../asahi/drm-shim/t8132_gallium_compute_mix4.c',
      '../../targets/dri/dri_target.c',
    ],
    include_directories: [inc_gallium_aux, inc_gallium, inc_include, inc_src,
                          inc_asahi, inc_loader, inc_gallium_winsys,
                          inc_gallium_drivers, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies: [driver_asahi, idep_nir, idep_mesautil, idep_libagx,
                   idep_libagx_shaders_h, dep_libdrm],
    link_with: [libpipe_loader_static, libws_null, libwsw, libswdri,
                libswkmsdri, libgallium],
    install : false,
  )

  if with_tests
    test(
      't8132_gallium_compute_xyz_oracle',
      t8132_gallium_compute_xyz,
      args : ['--self-test'],
      suite : ['asahi'],
    )

    test(
      't8132_gallium_compute_xy_oracle',
      t8132_gallium_compute_xy,
      args : ['--self-test'],
      suite : ['asahi'],
    )

    test(
      't8132_gallium_compute_device_index_oracle',
      t8132_gallium_compute_device_index,
      args : ['--self-test'],
      suite : ['asahi'],
    )

    test(
      't8132_gallium_compute_device_graph_oracle',
      t8132_gallium_compute_device_graph,
      args : ['--self-test'],
      suite : ['asahi'],
    )

  endif
endif
