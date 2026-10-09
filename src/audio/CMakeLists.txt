# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-audio
  src/game_audio.cpp
  src/unit_announcements.cpp
  src/wave_chunks.cpp
  src/mixer.cpp
  src/cd_music.cpp
  src/music_mood.cpp
  src/sound_system.cpp
  src/music_disc.cpp
  src/music_session.cpp
  src/spatial_gain.cpp
)
add_library(oa::audio ALIAS oa-audio)
target_include_directories(oa-audio PUBLIC include)
target_link_libraries(oa-audio PUBLIC oa-formats-hpi oa-options oa-core-types
  PRIVATE oa-base-bytes oa-formats-tdf oa-data-defs)

# The offline mix of the game's sound effects that director renders take
# their sound from: integer decoding, resampling and mixing, with no sound
# device and no clock.
add_library(oa-audio-offline-mix src/offline_mix.cpp)
add_library(oa::audio::offline_mix ALIAS oa-audio-offline-mix)
target_include_directories(oa-audio-offline-mix PUBLIC include)
target_link_libraries(oa-audio-offline-mix PUBLIC oa-audio PRIVATE oa-options)
target_compile_features(oa-audio-offline-mix PUBLIC cxx_std_20)
if(BUILD_TESTING)
  # Decoding, resampling, gains, the voice policy, chunked rendering and a
  # pinned mix; with --data, every sound file of the installed game.
  add_executable(oa-audio-offline-mix-test tests/offline_mix_test.cpp)
  target_link_libraries(oa-audio-offline-mix-test
    PRIVATE oa-audio-offline-mix oa-base-sha256 oa-test-game-data)
  add_test(NAME audio-offline-mix COMMAND oa-audio-offline-mix-test)
  oa_add_game_data_test(audio-offline-mix-data oa-audio-offline-mix-test --data)
endif()

# The music decoder: MP3, Ogg Vorbis, FLAC and WAVE files to 44.1 kHz
# stereo float, with the sample-rate converter it shares with the sound
# output. The decoders themselves are kept in third_party/ as their authors
# wrote them and compiled from music_codecs.c, in oa-audio-music-codecs,
# without the engine's warnings.
set(oa_music_codec_dirs "${PROJECT_SOURCE_DIR}/third_party/dr_libs"
  "${PROJECT_SOURCE_DIR}/third_party/stb_vorbis")
add_library(oa-audio-music-codecs OBJECT src/music_codecs.c)
add_library(oa::audio::music_codecs ALIAS oa-audio-music-codecs)
target_include_directories(oa-audio-music-codecs SYSTEM PRIVATE ${oa_music_codec_dirs})
if(MSVC)
  # Without oa-options: the decoders build their tables from constant
  # floating-point expressions, which /fp:strict does not take as static
  # initializers, and an option given after /W4 or /fp:strict would only
  # override it with a warning. Decoded music takes no part in a match.
  # /W0 turns every warning off, as /w does, and a generator that sets a
  # warning level of its own takes it as the target's level instead of
  # adding it after its own.
  target_compile_options(oa-audio-music-codecs PRIVATE /W0)
else()
  target_link_libraries(oa-audio-music-codecs PRIVATE oa-options)
  target_compile_options(oa-audio-music-codecs PRIVATE -w)
endif()
add_library(oa-audio-music-decoder src/music_decoder.cpp src/resampler.cpp)
add_library(oa::audio::music_decoder ALIAS oa-audio-music-decoder)
target_include_directories(oa-audio-music-decoder PUBLIC include)
target_include_directories(oa-audio-music-decoder SYSTEM PRIVATE ${oa_music_codec_dirs})
target_link_libraries(oa-audio-music-decoder PRIVATE oa-audio-music-codecs oa-base-bytes oa-platform-shims
  oa-options)
target_compile_features(oa-audio-music-decoder PUBLIC cxx_std_20)
if(NOT WIN32)
  # The Ogg Vorbis decoder calls the C maths library.
  find_library(OA_MATH_LIBRARY m)
  if(OA_MATH_LIBRARY)
    target_link_libraries(oa-audio-music-decoder PRIVATE ${OA_MATH_LIBRARY})
  endif()
endif()
if(BUILD_TESTING)
  # The resampler, the stereo mix, every encoding against the samples it
  # was made from, malformed files, and with --data the installed music.
  add_executable(oa-audio-music-decoder-test tests/music_decoder_test.cpp)
  target_link_libraries(oa-audio-music-decoder-test PRIVATE oa-audio-music-decoder oa-test-support
    oa-test-game-data oa-options)
  target_compile_definitions(oa-audio-music-decoder-test PRIVATE
    OA_AUDIO_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME audio-music-decoder COMMAND oa-audio-music-decoder-test)
  oa_add_game_data_test(audio-music-decoder-data oa-audio-music-decoder-test --data)
endif()

# The sound output every sound plays through (oa/audio/sound_output.hpp):
# SDL's where the build has SDL, and the engine's own mixer over the
# platform's sound device (src/platform/sound-device), which on Windows is
# the wave-out device and needs nothing newer than Windows XP. A build
# without SDL (OA_BUILD_PLATFORM off) gets the mixer alone.
add_library(oa-audio-output src/sound_output.cpp src/sound_output_none.cpp
  src/software_mixer.cpp src/buffered_output.cpp src/wave_out_output.cpp)
add_library(oa::audio::output ALIAS oa-audio-output)
target_include_directories(oa-audio-output PUBLIC include)
target_link_libraries(oa-audio-output PUBLIC oa-audio-music-decoder oa-platform-sound-device
  PRIVATE oa-platform-shims oa-options)
target_compile_features(oa-audio-output PUBLIC cxx_std_20)
if(OA_BUILD_PLATFORM)
  find_package(SDL3 3.2 CONFIG REQUIRED)
  target_sources(oa-audio-output PRIVATE src/sdl_sound_output.cpp)
  target_link_libraries(oa-audio-output PRIVATE SDL3::SDL3)
  target_compile_definitions(oa-audio-output PRIVATE OA_AUDIO_HAVE_SDL_OUTPUT=1)
endif()
if(BUILD_TESTING)
  # The integer resampler, the mixer's conversion, gains, feeds and
  # clamping, and the buffered output's ring of buffers on a device the
  # test plays by hand.
  add_executable(oa-audio-output-test tests/sound_output_test.cpp)
  target_link_libraries(oa-audio-output-test PRIVATE oa-audio-output oa-options oa-base-threads)
  add_test(NAME audio-output COMMAND oa-audio-output-test)
  if(WIN32)
    # The wave-out mixer on the system's own device: started, fed and
    # stopped. Where the system has no sound device it is skipped.
    add_test(NAME audio-output-wave-out COMMAND oa-audio-output-test --wave-out)
    set_tests_properties(audio-output-wave-out PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  endif()
endif()

if(OA_BUILD_PLATFORM)
  add_library(oa-audio-sdl src/sdl_audio.cpp src/sdl_music.cpp)
  add_library(oa::audio::sdl ALIAS oa-audio-sdl)
  target_include_directories(oa-audio-sdl PUBLIC include)
  target_link_libraries(oa-audio-sdl PUBLIC oa-audio oa-audio-output SDL3::SDL3 oa-options
    PRIVATE oa-audio-music-decoder)
endif()

if(BUILD_TESTING)
  add_executable(oa-game-audio-test tests/game_audio_test.cpp)
  target_link_libraries(oa-game-audio-test PRIVATE oa-audio oa-data-defs-assets oa-test-game-data)
  add_test(NAME game-audio COMMAND oa-game-audio-test)
  oa_add_game_data_test(game-audio-data oa-game-audio-test --data)

  foreach(test_name mixer cd_music speech_queue music_mood wave_chunks music_session)
    string(REPLACE "_" "-" test_id "${test_name}")
    add_executable(oa-audio-${test_id}-test tests/${test_name}_test.cpp)
    target_link_libraries(oa-audio-${test_id}-test PRIVATE oa-audio)
    add_test(NAME audio-${test_id} COMMAND oa-audio-${test_id}-test)
  endforeach()
  # Every WAV the installed game provides.
  target_link_libraries(oa-audio-wave-chunks-test PRIVATE oa-test-game-data)
  target_link_libraries(oa-audio-music-session-test PRIVATE oa-platform-shims oa-test-support)
  # The numbered MP3 files of the game folder OA_MOD_GAME_DIR names, with a
  # mod installed over it; skipped without it.
  add_test(NAME audio-music-session-mod-install COMMAND oa-audio-music-session-test --mod-install)
  set_tests_properties(audio-music-session-mod-install PROPERTIES SKIP_RETURN_CODE 77)
  if(DEFINED OA_MOD_GAME_DIR)
    set_tests_properties(audio-music-session-mod-install PROPERTIES
      ENVIRONMENT "OA_MOD_GAME_DIR=${OA_MOD_GAME_DIR}")
  endif()
  oa_add_game_data_test(audio-wave-chunks-data oa-audio-wave-chunks-test --data)
  if(OA_BUILD_PLATFORM)
    add_executable(oa-audio-sdl-sink-test tests/sdl_sink_test.cpp)
    target_link_libraries(oa-audio-sdl-sink-test PRIVATE oa-audio-sdl)
    add_test(NAME audio-sdl-sink COMMAND oa-audio-sdl-sink-test)
    set_tests_properties(audio-sdl-sink PROPERTIES TIMEOUT 60)
    # The WAV player's one stream: its delay, one playing, stopping at once
    # and replacement, on the dummy device, which plays in real time; and a
    # placed start's two sides, on a mixer the test runs by hand.
    add_executable(oa-audio-sdl-player-test tests/sdl_player_test.cpp)
    target_link_libraries(oa-audio-sdl-player-test PRIVATE oa-audio-sdl oa-test-support)
    add_test(NAME audio-sdl-player COMMAND oa-audio-sdl-player-test)
    set_tests_properties(audio-sdl-player PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
    add_executable(oa-audio-sdl-music-test tests/sdl_music_test.cpp)
    target_link_libraries(oa-audio-sdl-music-test PRIVATE oa-audio-sdl oa-test-game-data oa-test-support)
    add_test(NAME audio-sdl-music COMMAND oa-audio-sdl-music-test)
    add_test(NAME audio-sdl-music-data COMMAND oa-audio-sdl-music-test --data)
    set_tests_properties(audio-sdl-music audio-sdl-music-data PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
    # The installed game's music folder.
    oa_game_data_tests(audio-sdl-music-data)
  endif()
endif()
