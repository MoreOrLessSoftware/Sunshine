# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        version.lib)

# PyroWave is loaded at runtime from the executable's directory
if(SUNSHINE_PYROWAVE_DLL)
    add_custom_command(TARGET sunshine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SUNSHINE_PYROWAVE_DLL}" "$<TARGET_FILE_DIR:sunshine>"
            COMMENT "Copying PyroWave library")
endif()
