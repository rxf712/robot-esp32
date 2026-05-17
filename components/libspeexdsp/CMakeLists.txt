cmake_minimum_required(VERSION 3.16)

set(SPEEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/libspeexdsp")

set(SPEEXDSP_SOURCES
    ${SPEEX_DIR}/resample.c
    ${SPEEX_DIR}/preprocess.c
    ${SPEEX_DIR}/mdf.c
    ${SPEEX_DIR}/filterbank.c
    ${SPEEX_DIR}/fftwrap.c
    ${SPEEX_DIR}/smallft.c
    ${SPEEX_DIR}/kiss_fft.c
    ${SPEEX_DIR}/kiss_fftr.c
    ${SPEEX_DIR}/buffer.c
    ${SPEEX_DIR}/scal.c
    ${SPEEX_DIR}/jitter.c
)

idf_component_register(
    SRCS ${SPEEXDSP_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/include/speex"
        "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/libspeexdsp"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -DHAVE_CONFIG_H
    -DFLOATING_POINT
    -DUSE_KISS_FFT
    -w
)
