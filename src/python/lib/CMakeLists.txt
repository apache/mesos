if (ENABLE_NEW_CLI)
  add_test(
    NAME PythonLibTests
    COMMAND ${TOX} -c ${CMAKE_CURRENT_SOURCE_DIR}/tox.ini)
endif ()
