# Resets a downloaded checkout to its pinned commit and applies ${PATCH}. Used as the PATCH_COMMAND of the
# dsp56300 (cmake/dsp56300.cmake) and JUCE (src/plugin/CMakeLists.txt) downloads, which rerun whenever the patch
# file changes (its hash is part of the command), so the checkout always matches the patch exactly. ${RESET} is
# the part of the checkout the patch touches (reset with git checkout first). Only touches the checkouts CMake
# downloaded, never MNM_DSP56300_DIR.
find_package(Git REQUIRED)
if(NOT RESET)
  message(FATAL_ERROR "apply_patch.cmake: RESET (the path to reset) not given")
endif()
execute_process(COMMAND ${GIT_EXECUTABLE} checkout -- ${RESET} RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not reset ${RESET} in the checkout to patch with ${PATCH}")
endif()
execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${PATCH} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not apply ${PATCH}")
endif()
message(STATUS "applied ${PATCH}")
