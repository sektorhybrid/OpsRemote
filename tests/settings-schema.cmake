# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
#
# Every setting OpsRemote saves, with its default (OPSREMOTE_TEST_SETTINGS_SNAPSHOT),
# compared with tests/settings-migration/schema.yaml. A difference means a
# key or default changed: check whether old settings need a migration step
# (settingsmigrations.cpp), then accept it by copying the new snapshot over.
#   cmake -DAPP=<opsremote> -DEXPECTED=<schema.yaml> -DOUT=<new snapshot> -P settings-schema.cmake
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen "OPSREMOTE_TEST_SETTINGS_SNAPSHOT=${OUT}" "${APP}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "OpsRemote could not write the settings snapshot (${result}).")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${EXPECTED}" "${OUT}" RESULT_VARIABLE different)
if(different)
    message(FATAL_ERROR
        "The saved settings changed (a key or default):\n"
        "  diff -u ${EXPECTED} ${OUT}\n"
        "Is a migration step needed (renamed, moved, split, or a new default)? Then accept:\n"
        "  cp ${OUT} ${EXPECTED}")
endif()
