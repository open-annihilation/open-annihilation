# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# zstd 1.5.7, compiled unchanged from third_party/zstd, with its warnings off.
# Each vendored file is its own translation unit in this module, so the
# layout check does not follow the parent-relative includes inside those
# files. Assembly is left out (the copied .S is not built), threads are left
# off, and the legacy frame formats are left off. Nothing is fetched at build
# time.
set(oa_zstd_dir "${PROJECT_SOURCE_DIR}/third_party/zstd")
set(oa_zstd_sources
  src/vendor/vendor_common_debug.c
  src/vendor/vendor_common_entropy_common.c
  src/vendor/vendor_common_error_private.c
  src/vendor/vendor_common_fse_decompress.c
  src/vendor/vendor_common_pool.c
  src/vendor/vendor_common_threading.c
  src/vendor/vendor_common_xxhash.c
  src/vendor/vendor_common_zstd_common.c
  src/vendor/vendor_compress_fse_compress.c
  src/vendor/vendor_compress_hist.c
  src/vendor/vendor_compress_huf_compress.c
  src/vendor/vendor_compress_zstd_compress.c
  src/vendor/vendor_compress_zstd_compress_literals.c
  src/vendor/vendor_compress_zstd_compress_sequences.c
  src/vendor/vendor_compress_zstd_compress_superblock.c
  src/vendor/vendor_compress_zstd_double_fast.c
  src/vendor/vendor_compress_zstd_fast.c
  src/vendor/vendor_compress_zstd_lazy.c
  src/vendor/vendor_compress_zstd_ldm.c
  src/vendor/vendor_compress_zstd_opt.c
  src/vendor/vendor_compress_zstd_preSplit.c
  src/vendor/vendor_compress_zstdmt_compress.c
  src/vendor/vendor_decompress_huf_decompress.c
  src/vendor/vendor_decompress_zstd_ddict.c
  src/vendor/vendor_decompress_zstd_decompress.c
  src/vendor/vendor_decompress_zstd_decompress_block.c)
add_library(oa-base-zstd-vendor OBJECT ${oa_zstd_sources})
add_library(oa::base::zstd_vendor ALIAS oa-base-zstd-vendor)
target_include_directories(oa-base-zstd-vendor SYSTEM PRIVATE ${oa_zstd_dir})
target_compile_definitions(oa-base-zstd-vendor PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
if(MSVC)
  # /W0 turns every warning off. oa-options is left out: an option given after
  # the warning level it sets would not replace that level.
  target_compile_options(oa-base-zstd-vendor PRIVATE /W0)
else()
  target_link_libraries(oa-base-zstd-vendor PRIVATE oa-options)
  target_compile_options(oa-base-zstd-vendor PRIVATE -w)
endif()

add_library(oa-base-zstd src/zstd.cpp)
add_library(oa::base::zstd ALIAS oa-base-zstd)
target_include_directories(oa-base-zstd PUBLIC include)
target_include_directories(oa-base-zstd SYSTEM PRIVATE ${oa_zstd_dir})
target_compile_features(oa-base-zstd PUBLIC cxx_std_20)
target_link_libraries(oa-base-zstd PUBLIC oa-options PRIVATE oa-base-zstd-vendor)

if(BUILD_TESTING)
  add_executable(oa-base-zstd-test tests/zstd_test.cpp)
  # The test reads a frame's window with zstd's own header call.
  target_include_directories(oa-base-zstd-test SYSTEM PRIVATE ${oa_zstd_dir})
  target_link_libraries(oa-base-zstd-test PRIVATE oa-base-zstd oa-test-support oa-options)
  add_test(NAME base-zstd COMMAND oa-base-zstd-test)
  set_tests_properties(base-zstd PROPERTIES TIMEOUT 60)
endif()
