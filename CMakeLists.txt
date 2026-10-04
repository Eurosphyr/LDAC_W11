cmake_minimum_required(VERSION 3.12)
project(ldac-win C CXX)

set(BT ${CMAKE_SOURCE_DIR}/third_party/btstack)
set(LDAC ${CMAKE_SOURCE_DIR}/third_party/libldac)

# ---- BTstack (classic only) ----
file(GLOB BT_SRC     "${BT}/src/*.c")
file(GLOB BT_CLASSIC "${BT}/src/classic/*.c")
file(GLOB BT_WIN     "${BT}/platform/windows/*.c")
list(FILTER BT_WIN EXCLUDE REGEX "(btstack_uart_block_windows|hci_dump_windows_stdout)\\.c$")

set(BT_ALL ${BT_SRC} ${BT_CLASSIC} ${BT_WIN})
# files that need third-party libs we do not ship / not needed for A2DP
list(FILTER BT_ALL EXCLUDE REGEX "btstack_(sbc_bluedroid|lc3_google|lc3plc|lc3_fraunhofer|lc3plus_fraunhofer|audio_generator|resample|slip|network|stdin_support|uart_slip_wrapper|run_loop_embedded|run_loop_freertos|run_loop_freertos_single_thread|run_loop_base|hid_parser|tlv_none|mesh).*\\.c$")
list(FILTER BT_ALL EXCLUDE REGEX "/src/ble/")
list(FILTER BT_ALL EXCLUDE REGEX "/src/mesh/")
list(FILTER BT_ALL EXCLUDE REGEX "(hci_transport_h4|hci_transport_h5|hci_transport_em9301|hci_transport_zephyr)\\.c$")
list(FILTER BT_ALL EXCLUDE REGEX "(wav_util|btstack_audio_portaudio|btstack_audio_embedded|sdp_client_rfcomm)\\.c$")
list(FILTER BT_ALL EXCLUDE REGEX "(hxcmod|rijndael|md5|uECC)")
list(FILTER BT_ALL EXCLUDE REGEX "/src/classic/(hfp_msbc|hfp_codec|btstack_sbc|btstack_cvsd|btstack_lc3|btstack_hid|btstack_link_key_db_memory|btstack_cvsd_plc|btstack_sbc_plc)[^/]*\\.c$")
# btstack_run_loop_base.c is REQUIRED by the windows run loop; add it back
file(GLOB BT_RL_BASE "${BT}/src/btstack_run_loop_base.c")
list(APPEND BT_ALL ${BT_RL_BASE})
list(APPEND BT_ALL ${BT}/3rd-party/yxml/yxml.c ${BT}/3rd-party/md5/md5.c)
list(REMOVE_DUPLICATES BT_ALL)

add_library(btstack STATIC ${BT_ALL})
target_include_directories(btstack PUBLIC
    ${CMAKE_SOURCE_DIR}/src            # our btstack_config.h
    ${BT}/src ${BT}/src/classic ${BT}/platform/windows
    ${BT}/3rd-party/yxml ${BT}/3rd-party/md5 ${BT}/3rd-party/bluedroid/encoder/include ${BT}/3rd-party/bluedroid/decoder/include)
target_compile_options(btstack PRIVATE -w)

# ---- Sony libldac encoder (Apache-2.0) ----
add_library(ldac STATIC ${LDAC}/src/ldaclib.c ${LDAC}/src/ldacBT.c)
target_include_directories(ldac PUBLIC ${LDAC}/inc PRIVATE ${LDAC}/src)
target_compile_options(ldac PRIVATE -w -O2)

# ---- application ----
add_executable(ldac-win src/main.c src/audio_capture.cpp)
target_link_libraries(ldac-win btstack ldac setupapi winusb ole32 winmm uuid)
target_compile_options(ldac-win PRIVATE -O2)
target_link_options(ldac-win PRIVATE -static -static-libgcc -static-libstdc++)
