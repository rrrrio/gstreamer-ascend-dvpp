project('gstreamer-ascend-dvpp', 'c', version : '0.1.0',
  default_options : ['warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
gst_req = '>= 1.20.0'
gst = dependency('gstreamer-1.0', version : gst_req)
gst_base = dependency('gstreamer-base-1.0', version : gst_req)
gst_video = dependency('gstreamer-video-1.0', version : gst_req)
gst_allocators = dependency('gstreamer-allocators-1.0', version : gst_req)
glib = dependency('glib-2.0')

ascend_prefix = get_option('ascend_prefix')
use_ascend = get_option('with_ascend')
ascend_inc = []
ascend_lib = []
ascend_args = []
if use_ascend
  ascend_inc = include_directories(ascend_prefix / 'aarch64-linux/include')
  ascend_dvpp = cc.find_library('acl_dvpp', dirs : [ascend_prefix / 'aarch64-linux/lib64'])
  ascend_cl = cc.find_library('ascendcl', dirs : [ascend_prefix / 'aarch64-linux/lib64'])
  ascend_args += ['-DENABLE_DVPP_INTERFACE', '-DGST_ASCEND_HAVE_ACL=1']
  ascend_lib = [ascend_dvpp, ascend_cl]
else
  ascend_args += ['-DGST_ASCEND_HAVE_ACL=0']
endif

inc = include_directories('src')
sources = files(
  'src/gstascendplugin.c',
  'src/gstascendmemory.c',
  'src/gstascendruntime.c',
  'src/gstascendvideoconvert.c',
  'src/gstascendjpeg.c',
  'src/gstascendvideocodec.c',
)

plugin_target = shared_library('gstascenddvpp', sources,
  include_directories : [inc, ascend_inc],
  dependencies : [gst, gst_base, gst_video, gst_allocators, glib, ascend_lib],
  c_args : ascend_args,
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')

test_deps = [gst, gst_base, gst_video, gst_allocators, glib, ascend_lib]
test_env = environment()
if use_ascend
  test_env.prepend('LD_LIBRARY_PATH', ascend_prefix / 'aarch64-linux/lib64')
endif
if get_option('tests')
  test_exe = executable('test-ascend-memory', 'tests/test_ascend_memory.c',
    include_directories : [inc, ascend_inc],
    dependencies : test_deps,
    c_args : ascend_args,
    link_with : [],
    sources : ['src/gstascendmemory.c', 'src/gstascendruntime.c'])
  test('ascend-memory', test_exe, env : test_env, is_parallel : false)

  video_test_exe = executable('test-ascend-video-convert',
    'tests/test_ascend_video_convert.c',
    include_directories : [inc, ascend_inc],
    dependencies : test_deps,
    c_args : ascend_args,
    link_with : [],
    sources : [
      'src/gstascendmemory.c',
      'src/gstascendruntime.c',
      'src/gstascendvideoconvert.c',
    ])
  test('ascend-video-convert', video_test_exe, env : test_env,
    is_parallel : false)

  elements_test_exe = executable('test-ascend-elements',
    'tests/test_ascend_elements.c',
    include_directories : [inc, ascend_inc],
    dependencies : test_deps,
    c_args : ascend_args,
    link_with : [],
    sources : [
      'src/gstascendmemory.c',
      'src/gstascendruntime.c',
      'src/gstascendvideoconvert.c',
      'src/gstascendjpeg.c',
      'src/gstascendvideocodec.c',
    ])
  test('ascend-elements', elements_test_exe, env : test_env,
    is_parallel : false)

  crop_resize_test_exe = executable('test-ascend-crop-resize',
    'tests/test_ascend_crop_resize.c',
    include_directories : [inc, ascend_inc],
    dependencies : test_deps,
    c_args : ascend_args,
    link_with : [],
    sources : [
      'src/gstascendmemory.c',
      'src/gstascendruntime.c',
      'src/gstascendvideoconvert.c',
      'src/gstascendvideocodec.c',
    ])
  test('ascend-crop-resize', crop_resize_test_exe, env : test_env,
    is_parallel : false)

  plugin_test_exe = executable('test-ascend-plugin',
    'tests/test_ascend_plugin.c',
    include_directories : [inc, ascend_inc],
    dependencies : test_deps,
    c_args : ascend_args)
  test('ascend-plugin', plugin_test_exe, args : [plugin_target.full_path()],
    env : test_env, is_parallel : false)
endif

pkg = import('pkgconfig')
pkg.generate(name : 'gstreamer-ascend-dvpp',
  description : 'Ascend DVPP GStreamer hardware acceleration plugin',
  version : meson.project_version(),
  libraries : 'gstascenddvpp')
