cmake_minimum_required(VERSION 3.16)

set(FVAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libfvad")

set(FVAD_SOURCES
    ${FVAD_DIR}/src/fvad.c
    ${FVAD_DIR}/src/signal_processing/division_operations.c
    ${FVAD_DIR}/src/signal_processing/energy.c
    ${FVAD_DIR}/src/signal_processing/get_scaling_square.c
    ${FVAD_DIR}/src/signal_processing/resample_48khz.c
    ${FVAD_DIR}/src/signal_processing/resample_by_2_internal.c
    ${FVAD_DIR}/src/signal_processing/resample_fractional.c
    ${FVAD_DIR}/src/signal_processing/spl_inl.c
    ${FVAD_DIR}/src/vad/vad_core.c
    ${FVAD_DIR}/src/vad/vad_filterbank.c
    ${FVAD_DIR}/src/vad/vad_gmm.c
    ${FVAD_DIR}/src/vad/vad_sp.c
)

idf_component_register(
    SRCS ${FVAD_SOURCES}
    INCLUDE_DIRS
        "${FVAD_DIR}/include"
        "${FVAD_DIR}/src"
        "${FVAD_DIR}/src/signal_processing"
        "${FVAD_DIR}/src/vad"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)
