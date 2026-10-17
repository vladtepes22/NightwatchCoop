# Seamless regression targets, carried from visy v0.6.5.
enable_testing()
file(GLOB PARTY_NET_SOURCES CONFIGURE_DEPENDS shim/net/*.cpp)
set(PARTY_LINK_SOURCES shim/party/party_link.cpp shim/party/party_rudp.cpp
    shim/party/party_code.cpp shim/party/party_addr.cpp shim/party/party_crypto.cpp
    shim/party/party_lan.cpp shim/party/upnp_win.cpp)
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
    shim/party/party_nat.cpp shim/party/party_rdv.cpp shim/net/net_stun.cpp shim/net/netsim.cpp
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

add_executable(party-death-rules-test EXCLUDE_FROM_ALL ../tests/test_party_death_rules.cpp)

target_compile_options(party-death-rules-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

target_include_directories(party-death-rules-test PRIVATE shim)

set_target_properties(party-death-rules-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

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
    shim/party/party_desync.cpp shim/party/party_npctalk.cpp shim/party/party_ping.cpp)
add_executable(party-security-test EXCLUDE_FROM_ALL ${PARTY_FUZZ_SOURCES})
target_compile_definitions(party-security-test PRIVATE PARTY_FUZZ_QUICK
    BB_PARTY_TRAVEL_NO_GAME BB_PARTY_STORY_NO_GAME BB_PARTY_ITEMS_NO_GAME
    BB_PARTY_PHANTOM_NO_GAME BB_PARTY_PROGRESS_NO_GAME BB_PARTY_GUESTSVC_NO_GAME
    BB_PARTY_WORLDACT_NO_GAME BB_PARTY_NPCTALK_NO_GAME BB_PARTY_PING_NO_GAME
    BB_PARTY_CHALICE_NO_GAME BB_PARTY_DROPROLL_NO_GAME)
target_include_directories(party-security-test PRIVATE shim shim/net ..)
target_link_libraries(party-security-test PRIVATE ws2_32 iphlpapi ole32 oleaut32)
target_compile_options(party-security-test PRIVATE -UNDEBUG -Wall -Wextra)
set_target_properties(party-security-test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
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
