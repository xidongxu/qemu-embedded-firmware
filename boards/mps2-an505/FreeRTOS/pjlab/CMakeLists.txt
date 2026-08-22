# pjlab - pjproject (PJSIP) learning experiments.
#
# The pj_* experiment sources live here (src/, moved from application/);
# application/ is now reserved for application development.  main.c
# (application/) calls the experiment entry points (pj_test_run() etc.), so
# these sources are compiled into the same ${TARGET} and their headers are
# added to the target's include path.
#
# Entry points (called by application/main.c in order):
#   pj_test_run / pj_net_test_run / pj_sip_test_run /
#   pj_media_full_test_run / pj_media_dsp_test_run /
#   pj_sip_inv_test_run / pj_rtp_test_run / pj_call_test_run
#   (dual-QEMU / host-call: pj_sip_dual_test_run)

file(GLOB PJLAB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# pj_sip_dual_test.c is role-specific and #errors when neither role macro is
# defined; only compile it in dual mode (main.c runs only that test then).
if(PJ_DUAL_ROLE STREQUAL "none")
    list(REMOVE_ITEM PJLAB_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pj_sip_dual_test.c")
endif()

target_sources(${TARGET} PRIVATE ${PJLAB_SOURCES})
target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
