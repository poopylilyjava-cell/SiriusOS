# The desktop's sources, apart from each program's own main(): included by
# user/siriusd (for Sirius) and tests/host_harness (for the build machine).
set(SIRIUSD_DESKTOP_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/shell/desktop.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shell/settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shell/splash.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/apps/terminal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/apps/files.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/apps/editor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/apps/monitor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/apps/settings_app.cpp")
