# Programs that test the system from inside it. /bin/init runs them at boot.

sirius_program(cxxtest cxxtest.cpp)
sirius_program(ipctest ipctest.cpp)
sirius_program(threadtest threadtest.cpp)
sirius_program(ptytest ptytest.cpp)
sirius_program(wltest wltest.cpp)
