# Seamless regression targets, carried from visy v0.6.5.
enable_testing()
file(GLOB PARTY_NET_SOURCES CONFIGURE_DEPENDS shim/net/*.cpp)
set(PARTY_LINK_SOURCES shim/party/party_link.cpp shim/party/party_rudp.cpp
    shim/party/party_code.cpp shim/party/party_addr.cpp shim/party/party_crypto.cpp
    shim/party/party_lan.cpp shim/party/upnp_win.cpp shim/party/party_nat.cpp)
add_executable(party-net-test EXCLUDE_FROM_ALL ../tests/test_party_net.cpp ${PARTY_NET_SOURCES})

target_compile_options(party-net-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-net-test PRIVATE shim/net ..)

set_target_properties(party-net-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-host-test EXCLUDE_FROM_ALL ../tests/test_party_host.cpp ${PARTY_NET_SOURCES})

target_compile_options(party-host-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-host-test PRIVATE shim/net ..)

set_target_properties(party-host-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-link-test EXCLUDE_FROM_ALL ../tests/test_party_link.cpp ${PARTY_LINK_SOURCES}
    shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)

target_compile_options(party-link-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-link-test PRIVATE shim)

set_target_properties(party-link-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-nat-test EXCLUDE_FROM_ALL ../tests/test_party_nat.cpp ${PARTY_LINK_SOURCES}
    shim/party/party_rdv.cpp shim/party/party_netpath.cpp shim/net/party_route.cpp
    shim/net/party_udp.cpp shim/net/net_stun.cpp shim/net/netsim.cpp
    third_party/monocypher/monocypher.c)

target_compile_options(party-nat-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-nat-test PRIVATE shim)

set_target_properties(party-nat-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-collision-test EXCLUDE_FROM_ALL ../tests/test_party_collision.cpp)
target_include_directories(party-collision-test PRIVATE shim)
target_compile_options(party-collision-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
set_target_properties(party-collision-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-flow-test EXCLUDE_FROM_ALL ../tests/test_party_flow.cpp shim/party/party_flow.cpp shim/net/json.cpp)

target_compile_options(party-flow-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-flow-test PRIVATE shim)

set_target_properties(party-flow-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-start-test EXCLUDE_FROM_ALL ../tests/test_party_start.cpp shim/party/party_start.cpp)

target_compile_options(party-start-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-start-test PRIVATE shim)

set_target_properties(party-start-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-dsummon-test EXCLUDE_FROM_ALL ../tests/test_party_dsummon.cpp shim/party/party_dsummon.cpp)

target_compile_options(party-dsummon-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-dsummon-test PRIVATE shim)

set_target_properties(party-dsummon-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-newgame-test EXCLUDE_FROM_ALL ../tests/test_party_newgame.cpp shim/party/test_newgame.cpp)

target_compile_definitions(party-newgame-test PRIVATE BB_TEST_NEWGAME_NO_GAME)

target_compile_options(party-newgame-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-newgame-test PRIVATE shim)

set_target_properties(party-newgame-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-watchdog-test EXCLUDE_FROM_ALL ../tests/test_party_watchdog.cpp shim/party/party_watchdog.cpp)

target_compile_options(party-watchdog-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-watchdog-test PRIVATE shim)

set_target_properties(party-watchdog-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-log-test EXCLUDE_FROM_ALL ../tests/test_party_log.cpp shim/party/party_log.cpp
    shim/party/party_async_io.cpp)

target_compile_options(party-log-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-log-test PRIVATE shim)

set_target_properties(party-log-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-env-test EXCLUDE_FROM_ALL ../tests/test_party_env.cpp)

target_compile_options(party-env-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

set_target_properties(party-env-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-stick-test EXCLUDE_FROM_ALL ../tests/test_party_stick.cpp)

target_compile_options(party-stick-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-stick-test PRIVATE shim)

set_target_properties(party-stick-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-spectate-test EXCLUDE_FROM_ALL ../tests/test_party_spectate.cpp)

target_compile_options(party-spectate-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-spectate-test PRIVATE shim)

set_target_properties(party-spectate-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-spectator-hide-test EXCLUDE_FROM_ALL ../tests/test_party_spectator_hide.cpp)

target_compile_options(party-spectator-hide-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-spectator-hide-test PRIVATE shim)

set_target_properties(party-spectator-hide-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-gather-test EXCLUDE_FROM_ALL ../tests/test_party_gather.cpp)
target_compile_options(party-gather-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-gather-test PRIVATE shim)
set_target_properties(party-gather-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-remote-show-test EXCLUDE_FROM_ALL ../tests/test_party_remote_show.cpp)
target_compile_options(party-remote-show-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-remote-show-test PRIVATE shim)
set_target_properties(party-remote-show-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-npc-mirror-test EXCLUDE_FROM_ALL ../tests/test_party_npc_mirror.cpp)
target_compile_options(party-npc-mirror-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npc-mirror-test PRIVATE shim)
set_target_properties(party-npc-mirror-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# 0.7.3 lamp travel voting (party_vote_rules.h). ninja -C out/gpu party-vote-test
add_executable(party-vote-test EXCLUDE_FROM_ALL ../tests/test_party_vote.cpp)
target_compile_options(party-vote-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-vote-test PRIVATE shim)
set_target_properties(party-vote-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# NPC hunter fights with the whole party (party_npcfight_rules.h, header only). ninja -C out/gpu party-npcfight-test
add_executable(party-npcfight-test EXCLUDE_FROM_ALL ../tests/test_party_npcfight.cpp)
target_compile_options(party-npcfight-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npcfight-test PRIVATE shim)
set_target_properties(party-npcfight-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# 0.7.3 "Anyone can be abducted" (party_kidnap_rules.h, header only). ninja -C out/gpu party-kidnap-test
add_executable(party-kidnap-test EXCLUDE_FROM_ALL ../tests/test_party_kidnap.cpp)
target_compile_options(party-kidnap-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-kidnap-test PRIVATE shim)
set_target_properties(party-kidnap-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# 0.7.5 guests' blows in the host's damage records (party_npchit_rules.h, header only).
# ninja -C out/gpu party-npchit-test
add_executable(party-npchit-test EXCLUDE_FROM_ALL ../tests/test_party_npchit.cpp)
target_compile_options(party-npchit-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-npchit-test PRIVATE shim)
set_target_properties(party-npchit-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-death-rules-test EXCLUDE_FROM_ALL ../tests/test_party_death_rules.cpp)

target_compile_options(party-death-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-death-rules-test PRIVATE shim)

set_target_properties(party-death-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# Split travel notice text + rate limit (party_split_notice.h; visy 0.7.4).
add_executable(party-split-notice-test EXCLUDE_FROM_ALL ../tests/test_party_split_notice.cpp)
target_compile_options(party-split-notice-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-split-notice-test PRIVATE shim)
set_target_properties(party-split-notice-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-guestvals-test EXCLUDE_FROM_ALL ../tests/test_party_guestvals.cpp)

target_compile_options(party-guestvals-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-guestvals-test PRIVATE shim)

set_target_properties(party-guestvals-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-enemy-census-test EXCLUDE_FROM_ALL ../tests/test_party_enemy_census.cpp)

target_compile_options(party-enemy-census-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-enemy-census-test PRIVATE shim)

set_target_properties(party-enemy-census-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-nr-drop-test EXCLUDE_FROM_ALL ../tests/test_party_nr_drop.cpp)

target_compile_options(party-nr-drop-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-nr-drop-test PRIVATE shim)

set_target_properties(party-nr-drop-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-progress-test EXCLUDE_FROM_ALL ../tests/test_party_progress.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)

target_compile_definitions(party-progress-test PRIVATE BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

target_compile_options(party-progress-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-progress-test PRIVATE shim)

set_target_properties(party-progress-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-desync-test EXCLUDE_FROM_ALL ../tests/test_party_desync.cpp shim/party/party_desync.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)

target_compile_definitions(party-desync-test PRIVATE BB_PARTY_PROGRESS_NO_GAME)

target_compile_options(party-desync-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-desync-test PRIVATE shim)

set_target_properties(party-desync-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-host-harness EXCLUDE_FROM_ALL ../tests/party_host_harness.cpp ${PARTY_NET_SOURCES}
    ${PARTY_LINK_SOURCES} third_party/monocypher/monocypher.c)

target_compile_options(party-host-harness PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-host-harness PRIVATE shim shim/net ..)

set_target_properties(party-host-harness PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-status-test EXCLUDE_FROM_ALL ../tests/test_party_status.cpp shim/party/party_status.cpp)

target_compile_options(party-status-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-status-test PRIVATE shim)

set_target_properties(party-status-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# Connection warnings (shim/party/party_connwarn.cpp): firewall rule classification, thresholds, seam.
add_executable(party-connwarn-test EXCLUDE_FROM_ALL ../tests/test_party_connwarn.cpp shim/party/party_connwarn.cpp
    shim/party/party_status.cpp)
target_compile_options(party-connwarn-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-connwarn-test PRIVATE shim)
set_target_properties(party-connwarn-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# Chalice Dungeon progress brought home (party_chalice_merge.h). ninja -C out/gpu party-chalice-merge-test
add_executable(party-chalice-merge-test EXCLUDE_FROM_ALL ../tests/test_party_chalice_merge.cpp)
target_compile_options(party-chalice-merge-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-chalice-merge-test PRIVATE shim)
set_target_properties(party-chalice-merge-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-soak EXCLUDE_FROM_ALL ../tests/party_soak.cpp ${PARTY_NET_SOURCES} ${PARTY_LINK_SOURCES}
    third_party/monocypher/monocypher.c)

target_compile_options(party-soak PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-soak PRIVATE shim/net shim ..)

set_target_properties(party-soak PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)


add_executable(party-balance-test EXCLUDE_FROM_ALL ../tests/test_party_balance.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)

target_compile_options(party-balance-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-balance-test PRIVATE shim)

set_target_properties(party-balance-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-pvp-test EXCLUDE_FROM_ALL ../tests/test_party_pvp.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)

target_compile_options(party-pvp-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-pvp-test PRIVATE shim)

set_target_properties(party-pvp-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-hud-test EXCLUDE_FROM_ALL ../tests/test_party_hud.cpp shim/party/party_hud.cpp
    ${PARTY_LINK_SOURCES} shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)

target_compile_options(party-hud-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-hud-test PRIVATE shim)

set_target_properties(party-hud-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-status-bridge-test EXCLUDE_FROM_ALL ../tests/test_party_status_bridge.cpp
    shim/party/party_status_bridge.cpp shim/party/party_status.cpp shim/party/party_async_io.cpp
    ${PARTY_LINK_SOURCES} shim/net/net_stun.cpp shim/net/netsim.cpp third_party/monocypher/monocypher.c)

target_compile_definitions(party-status-bridge-test PRIVATE BB_PARTY_BRIDGE_NO_GAME)

target_compile_options(party-status-bridge-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-status-bridge-test PRIVATE shim)

set_target_properties(party-status-bridge-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-travel-test EXCLUDE_FROM_ALL ../tests/test_party_travel.cpp shim/party/party_travel.cpp
    shim/party/party_chalice.cpp shim/net/json.cpp)

target_compile_definitions(party-travel-test PRIVATE BB_PARTY_TRAVEL_NO_GAME BB_PARTY_CHALICE_NO_GAME)

target_compile_options(party-travel-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-travel-test PRIVATE shim)

set_target_properties(party-travel-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-story-test EXCLUDE_FROM_ALL ../tests/test_party_story.cpp shim/party/party_story.cpp
    shim/net/json.cpp)

target_compile_definitions(party-story-test PRIVATE BB_PARTY_STORY_NO_GAME)

target_compile_options(party-story-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-story-test PRIVATE shim)

set_target_properties(party-story-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-items-test EXCLUDE_FROM_ALL ../tests/test_party_items.cpp shim/party/party_items.cpp
    shim/net/json.cpp)

target_compile_definitions(party-items-test PRIVATE BB_PARTY_ITEMS_NO_GAME)

target_compile_options(party-items-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-items-test PRIVATE shim)

set_target_properties(party-items-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-phantom-test EXCLUDE_FROM_ALL ../tests/test_party_phantom.cpp shim/party/party_phantom.cpp
    shim/party/party_travel.cpp shim/net/json.cpp)

target_compile_definitions(party-phantom-test PRIVATE BB_PARTY_PHANTOM_NO_GAME BB_PARTY_TRAVEL_NO_GAME)

target_compile_options(party-phantom-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-phantom-test PRIVATE shim)

set_target_properties(party-phantom-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-guestsvc-test EXCLUDE_FROM_ALL ../tests/test_party_guestsvc.cpp shim/party/party_guestsvc.cpp
    shim/party/party_phantom.cpp shim/party/party_travel.cpp shim/net/json.cpp)

target_compile_definitions(party-guestsvc-test PRIVATE BB_PARTY_GUESTSVC_NO_GAME BB_PARTY_PHANTOM_NO_GAME
    BB_PARTY_TRAVEL_NO_GAME)

target_compile_options(party-guestsvc-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-guestsvc-test PRIVATE shim)

set_target_properties(party-guestsvc-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-worldact-test EXCLUDE_FROM_ALL ../tests/test_party_worldact.cpp shim/party/party_worldact.cpp
    shim/party/party_progress.cpp shim/net/json.cpp)

target_compile_definitions(party-worldact-test PRIVATE BB_PARTY_WORLDACT_NO_GAME BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

target_compile_options(party-worldact-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-worldact-test PRIVATE shim)

set_target_properties(party-worldact-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-npctalk-test EXCLUDE_FROM_ALL ../tests/test_party_npctalk.cpp shim/party/party_npctalk.cpp
    shim/net/json.cpp)

target_compile_definitions(party-npctalk-test PRIVATE BB_PARTY_NPCTALK_NO_GAME)

target_compile_options(party-npctalk-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-npctalk-test PRIVATE shim)

set_target_properties(party-npctalk-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-ping-test EXCLUDE_FROM_ALL ../tests/test_party_ping.cpp shim/party/party_ping.cpp
    shim/net/json.cpp)

target_compile_options(party-ping-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-ping-test PRIVATE shim)

set_target_properties(party-ping-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-droproll-test EXCLUDE_FROM_ALL ../tests/test_party_droproll.cpp shim/party/party_droproll.cpp
    shim/net/json.cpp)

target_compile_definitions(party-droproll-test PRIVATE BB_PARTY_DROPROLL_NO_GAME)

target_compile_options(party-droproll-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-droproll-test PRIVATE shim)

set_target_properties(party-droproll-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

add_executable(party-fourp-test EXCLUDE_FROM_ALL ../tests/test_party_fourp.cpp shim/party/party_fourp.cpp)

target_compile_definitions(party-fourp-test PRIVATE BB_PARTY_FOURP_NO_GAME)

target_compile_options(party-fourp-test PRIVATE -UNDEBUG -Wall -Wextra)

target_include_directories(party-fourp-test PRIVATE shim)

set_target_properties(party-fourp-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

set(PARTY_FUZZ_SOURCES ../tests/party_fuzz.cpp ${PARTY_NET_SOURCES} ${PARTY_LINK_SOURCES}
    third_party/monocypher/monocypher.c shim/party/party_travel.cpp shim/party/party_story.cpp
    shim/party/party_items.cpp shim/party/party_phantom.cpp shim/party/party_progress.cpp
    shim/party/party_guestsvc.cpp shim/party/party_worldact.cpp shim/party/party_balance_rules.cpp
    shim/party/party_chalice.cpp shim/party/party_droproll.cpp shim/party/party_dsummon.cpp
    shim/party/party_desync.cpp shim/party/party_npctalk.cpp shim/party/party_ping.cpp
    shim/party/party_gestures.cpp shim/party/party_rdv.cpp)
add_executable(party-security-test EXCLUDE_FROM_ALL ${PARTY_FUZZ_SOURCES})
target_compile_definitions(party-security-test PRIVATE PARTY_FUZZ_QUICK
    BB_PARTY_TRAVEL_NO_GAME BB_PARTY_STORY_NO_GAME BB_PARTY_ITEMS_NO_GAME
    BB_PARTY_PHANTOM_NO_GAME BB_PARTY_PROGRESS_NO_GAME BB_PARTY_GUESTSVC_NO_GAME
    BB_PARTY_WORLDACT_NO_GAME BB_PARTY_NPCTALK_NO_GAME BB_PARTY_PING_NO_GAME
    BB_PARTY_CHALICE_NO_GAME BB_PARTY_DROPROLL_NO_GAME BB_PARTY_GESTURES_NO_GAME)
target_include_directories(party-security-test PRIVATE shim shim/net ..)
target_link_libraries(party-security-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
target_compile_options(party-security-test PRIVATE -UNDEBUG -Wall -Wextra)
set_target_properties(party-security-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Party gestures (shim/party/party_gestures.cpp; in bbgpu through SHIM_SOURCES): ids, JSON, the
# owed store and the decision (BB_PARTY_GESTURES_NO_GAME). ninja -C out/gpu party-gestures-test
add_executable(party-gestures-test EXCLUDE_FROM_ALL ../tests/test_party_gestures.cpp shim/party/party_gestures.cpp
    shim/net/json.cpp)
target_compile_definitions(party-gestures-test PRIVATE BB_PARTY_GESTURES_NO_GAME)
target_compile_options(party-gestures-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-gestures-test PRIVATE shim)
set_target_properties(party-gestures-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Lifts in a party: the table and the decisions (party_lift_rules.h). ninja -C out/gpu party-lift-rules-test
add_executable(party-lift-rules-test EXCLUDE_FROM_ALL ../tests/test_party_lift_rules.cpp)
target_compile_options(party-lift-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-lift-rules-test PRIVATE shim)
set_target_properties(party-lift-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# A player's own world is built with chr type 0 (party_ownworld_rules.h). ninja -C out/gpu party-ownworld-rules-test
add_executable(party-ownworld-rules-test EXCLUDE_FROM_ALL ../tests/test_party_ownworld_rules.cpp)
target_compile_options(party-ownworld-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-ownworld-rules-test PRIVATE shim)
set_target_properties(party-ownworld-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# A guest the party keeps where it stands (party_stay_rules.h). ninja -C out/gpu party-stay-rules-test
add_executable(party-stay-rules-test EXCLUDE_FROM_ALL ../tests/test_party_stay_rules.cpp)
target_compile_options(party-stay-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-stay-rules-test PRIVATE shim)
set_target_properties(party-stay-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Companions without the phantom glow (party_glow_rules.h). ninja -C out/gpu party-glow-test
add_executable(party-glow-test EXCLUDE_FROM_ALL ../tests/test_party_glow.cpp)
target_compile_options(party-glow-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-glow-test PRIVATE shim)
set_target_properties(party-glow-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# The open party's join rule (party_open_rules.h). ninja -C out/gpu party-open-test
add_executable(party-open-test EXCLUDE_FROM_ALL ../tests/test_party_open.cpp)
target_compile_options(party-open-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-open-test PRIVATE shim)
set_target_properties(party-open-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# "Stay in my world" (party_stayhome_rules.h). ninja -C out/gpu party-stayhome-test
add_executable(party-stayhome-test EXCLUDE_FROM_ALL ../tests/test_party_stayhome.cpp)
target_compile_options(party-stayhome-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-stayhome-test PRIVATE shim)
set_target_properties(party-stayhome-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Splitscreen mode: messages filter, compass-only HUD (party_splitscreen_rules.h). ninja -C out/gpu party-splitscreen-test
add_executable(party-splitscreen-test EXCLUDE_FROM_ALL ../tests/test_party_splitscreen.cpp)
target_compile_options(party-splitscreen-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-splitscreen-test PRIVATE shim)
set_target_properties(party-splitscreen-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Level sync and phantom health (party_levelsync_rules.h). ninja -C out/gpu party-levelsync-test
add_executable(party-levelsync-test EXCLUDE_FROM_ALL ../tests/test_party_levelsync.cpp)
target_compile_options(party-levelsync-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-levelsync-test PRIVATE shim)
set_target_properties(party-levelsync-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Boot watchdog (shim/bbport_boot_watch.h, BB_BOOT_WATCHDOG): the rules. ninja -C out/gpu boot-watch-test
add_executable(boot-watch-test EXCLUDE_FROM_ALL ../tests/test_boot_watch.cpp)
target_compile_options(boot-watch-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(boot-watch-test PRIVATE shim)
set_target_properties(boot-watch-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# C4 rot cleanses (0.7.2, shim/party/party_rot_rules.h, header only) with the rules / party_rot.txt
# fields of party_balance_rules. cmake --build out/gpu --target party-rot-test
add_executable(party-rot-test EXCLUDE_FROM_ALL ../tests/test_party_rot.cpp shim/party/party_balance_rules.cpp
    shim/net/json.cpp)
target_compile_options(party-rot-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-rot-test PRIVATE shim)
set_target_properties(party-rot-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
# Boss fights after a party load (party_bossfog_rules.h), over every boss fog (party_worldact.cpp).
# cmake --build out/gpu --target party-bossfog-rules-test
add_executable(party-bossfog-rules-test EXCLUDE_FROM_ALL ../tests/test_party_bossfog_rules.cpp
    shim/party/party_worldact.cpp shim/party/party_progress.cpp shim/net/json.cpp)
target_compile_definitions(party-bossfog-rules-test PRIVATE BB_PARTY_WORLDACT_NO_GAME BB_PARTY_PROGRESS_NO_GAME
    BB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(party-bossfog-rules-test PRIVATE -UNDEBUG -Wall -Wextra)
target_include_directories(party-bossfog-rules-test PRIVATE shim)
set_target_properties(party-bossfog-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
add_test(NAME boot-watch-test COMMAND boot-watch-test)
# Who runs an enemy's AI on a guest's game, the limbo boss target (party_enemyai_rules.h). ninja -C out/gpu party-enemyai-rules-test
add_executable(party-enemyai-rules-test EXCLUDE_FROM_ALL ../tests/test_party_enemyai_rules.cpp)
target_compile_options(party-enemyai-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-enemyai-rules-test PRIVATE shim)
set_target_properties(party-enemyai-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

get_property(all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(party_tests)
foreach(target IN LISTS all_targets)
    if(target MATCHES "^party-.*-test$")
        target_link_libraries(${target} PRIVATE ws2_32 iphlpapi ole32 oleaut32 crypt32 synchronization)
        list(APPEND party_tests ${target})
        add_test(NAME ${target} COMMAND ${target})
    endif()
endforeach()
add_custom_target(seamless-tests DEPENDS ${party_tests} party-soak party-host-harness)
add_dependencies(seamless-tests boot-watch-test)

target_link_libraries(party-soak PRIVATE ws2_32 iphlpapi ole32 oleaut32 crypt32 synchronization winmm)
target_link_libraries(party-host-harness PRIVATE ws2_32 iphlpapi ole32 oleaut32 crypt32 synchronization winmm)
# The allocation counter overrides new/delete; keep libc++ allocations in the same module.
target_link_options(party-security-test PRIVATE -static-libstdc++)

add_executable(save-policy-test EXCLUDE_FROM_ALL ../tests/test_save_policy.c ../src/runtime_savepolicy.c)
target_include_directories(save-policy-test PRIVATE ../src)
target_compile_options(save-policy-test PRIVATE -UNDEBUG)
add_test(NAME save-policy-test COMMAND save-policy-test)
add_dependencies(seamless-tests save-policy-test)
target_link_libraries(save-policy-test PRIVATE winpthread)

add_executable(atomic-save-test EXCLUDE_FROM_ALL ../tests/test_atomic_save.c ../src/win32_compat.c)
target_include_directories(atomic-save-test PRIVATE ../src ..)
target_compile_options(atomic-save-test PRIVATE -UNDEBUG)
target_compile_definitions(atomic-save-test PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(atomic-save-test PRIVATE ws2_32)
add_test(NAME atomic-save-test COMMAND atomic-save-test)
add_dependencies(seamless-tests atomic-save-test)
set_tests_properties(party-link-test PROPERTIES TIMEOUT 180)
set_tests_properties(party-security-test PROPERTIES TIMEOUT 90)

add_executable(seamless-launcher-test EXCLUDE_FROM_ALL ../tests/test_seamless_launcher.cpp shim/seamless_launcher.cpp shim/party/party_status.cpp shim/net/json.cpp)
target_include_directories(seamless-launcher-test PRIVATE shim)
target_compile_options(seamless-launcher-test PRIVATE -UNDEBUG)
add_test(NAME seamless-launcher-test COMMAND seamless-launcher-test)
add_dependencies(seamless-tests seamless-launcher-test)

add_executable(game-menu-rows-test EXCLUDE_FROM_ALL ../tests/test_game_menu_rows.cpp
    shim/bbport_game_menu_pages.cpp shim/party/party_npctalk.cpp shim/party/party_flow_rows.cpp
    shim/bbport_settings.cpp shim/bbport_menu_i18n.cpp shim/party/party_status.cpp shim/party/party_balance_rules.cpp shim/net/json.cpp)
target_compile_definitions(game-menu-rows-test PRIVATE BB_PARTY_NPCTALK_NO_GAME)
target_compile_options(game-menu-rows-test PRIVATE -UNDEBUG)
target_include_directories(game-menu-rows-test PRIVATE .. shim)
set_target_properties(game-menu-rows-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
add_test(NAME game-menu-rows-test COMMAND game-menu-rows-test)
add_dependencies(seamless-tests game-menu-rows-test)

# Low host memory with Windows refusing commits (src/win32_memory.c).
if (WIN32)
    add_executable(win32-memory-test EXCLUDE_FROM_ALL ../tests/test_win32_memory.c ../src/win32_memory.c)
    target_include_directories(win32-memory-test PRIVATE ../src)
    target_compile_options(win32-memory-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    set_target_properties(win32-memory-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    add_test(NAME win32-memory-test COMMAND win32-memory-test)
    add_dependencies(seamless-tests win32-memory-test)
endif()

# Controller input and remapping (src/runtime_pad.c, pad.<input>= in bbport.ini) through SDL's
# virtual controller: every launcher button and input known, every PS4 button remapped at once.
# build.sh --test builds the same file; this one runs on Windows too.
add_executable(pad-test EXCLUDE_FROM_ALL ../tests/test_pad.c)
target_include_directories(pad-test PRIVATE .. ../src)
target_link_libraries(pad-test PRIVATE PkgConfig::SDL3 Threads::Threads)
target_compile_options(pad-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
set_target_properties(pad-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
add_test(NAME pad-test COMMAND pad-test)
add_dependencies(seamless-tests pad-test)

# visy builds everything with WIN32_LEAN_AND_MEAN: without it <windows.h> brings in winsock, whose
# h_addr macro breaks shim/net/party_route.cpp (Router::Client::h_addr, 0.7.1). The test targets
# compiling it get the define here.
get_property(_seamless_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _seamless_targets)
    get_target_property(_sources ${_target} SOURCES)
    if(NOT _target STREQUAL "bbgpu" AND "${_sources}" MATCHES "party_route\.cpp")
        target_compile_definitions(${_target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endforeach()

# The in-game map page's rules (area per game map, zoom): gpu/shim/map/map_page_rules.h.
add_executable(map-page-test EXCLUDE_FROM_ALL ../tests/test_map_page.cpp)
target_include_directories(map-page-test PRIVATE shim)
target_compile_options(map-page-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME map-page-test COMMAND map-page-test)
add_dependencies(seamless-tests map-page-test)

# Direct lamp travel's and the map's travel rules: gpu/shim/party/lamp_travel_rules.h.
add_executable(lamp-travel-test EXCLUDE_FROM_ALL ../tests/test_lamp_travel.cpp)
target_include_directories(lamp-travel-test PRIVATE shim)
target_compile_options(lamp-travel-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME lamp-travel-test COMMAND lamp-travel-test)
add_dependencies(seamless-tests lamp-travel-test)

# The texture collector's marks, levels and per-frame quota (texture_cache/gc_policy.h; visy 90a18103).
add_executable(gc-policy-test EXCLUDE_FROM_ALL ../tests/test_gc_policy.cpp)
target_include_directories(gc-policy-test PRIVATE ..)
target_compile_options(gc-policy-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME gc-policy-test COMMAND gc-policy-test)
add_dependencies(seamless-tests gc-policy-test)
