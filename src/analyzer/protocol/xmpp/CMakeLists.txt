
include(ZeekPlugin)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

zeek_plugin_begin(Zeek XMPP)
zeek_plugin_cc(Plugin.cc)
zeek_plugin_cc(XMPP.cc)
zeek_plugin_bif(events.bif)
zeek_plugin_pac(xmpp.pac xmpp-analyzer.pac xmpp-protocol.pac)
zeek_plugin_end()

