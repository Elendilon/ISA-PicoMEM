add_library(ne2000 INTERFACE)

#if(NOT PICO_BOARD STREQUAL "pico_w")
#    message(FATAL_ERROR "NE2000 Requires Pico W")
#endif()

target_include_directories(ne2000 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ne2000
)

target_sources(ne2000 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/ne2000.c
    ${CMAKE_CURRENT_LIST_DIR}/pm_net.c
    ${CMAKE_CURRENT_LIST_DIR}/pm_ftp.c
)

#target_link_libraries(ne2000 INTERFACE
#    pico_cyw43_arch_lwip_threadsafe_background
#)

target_link_libraries(ne2000 INTERFACE
     pico_cyw43_arch_none
)

# On board TCP/IP stack (FTP server).
#
# We deliberately keep pico_cyw43_arch_none and link lwIP on its own rather than
# using pico_cyw43_arch_lwip_*. Those variants build cyw43_lwip.c, which defines
# cyw43_cb_process_ethernet() and would clash with the NE2000 bridge that owns
# that symbol. Linking plain lwIP lets pm_net.c create its own netif and keeps
# the frame dispatching in ne2000.c.
#
# pico_lwip     : core + core4 + netif (ethernet_input, etharp_output...)
# pico_lwip_nosys : NO_SYS glue, provides sys_now(). It also links pico_lwip,
#                 both are listed to make the dependency explicit.
# Note that we do NOT call lwip_nosys_init() : it would run sys_check_timeouts()
# from the async_context (IRQ), while pm_net.c runs it from the Core 0 loop.
if (USE_PM_NET)
    target_link_libraries(ne2000 INTERFACE
        pico_lwip
        pico_lwip_nosys
    )
endif()
