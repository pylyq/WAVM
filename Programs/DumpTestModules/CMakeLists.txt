add_executable(DumpTestModules DumpTestModules.cpp)
target_link_libraries(DumpTestModules Logging IR WAST)
set_target_properties(DumpTestModules PROPERTIES FOLDER Testing)
