cmake_minimum_required(VERSION 3.20)

project(UnrealMappingsDumper LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Only 64 bit builds are supported, Unreal Engine games are 64 bit.")
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "Only Windows and Linux are supported.")
endif()

option(UMD_BUILD_TESTS "Build the fake engine tests" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DUMPER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/UnrealMappingsDumper)

add_library(DumperCore STATIC
	${DUMPER_DIR}/app.cpp
	${DUMPER_DIR}/dumper.cpp
	${DUMPER_DIR}/log.cpp
	${DUMPER_DIR}/memory.cpp
	${DUMPER_DIR}/names.cpp
	${DUMPER_DIR}/scanning.cpp
	${DUMPER_DIR}/unrealVersion.cpp
)

target_include_directories(DumperCore PUBLIC ${DUMPER_DIR})

if(MSVC)
	target_compile_options(DumperCore PUBLIC /EHa /utf-8 /permissive- /wd4244 /wd4267)
	set_property(TARGET DumperCore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
	target_compile_options(DumperCore PUBLIC -Wall -Wno-unknown-pragmas -Wno-sign-compare)
endif()

function(umd_static_runtime target)
	if(MINGW)
		target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
	elseif(MSVC)
		set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	else()
		target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
	endif()
endfunction()

if(WIN32)
	target_sources(DumperCore PRIVATE ${DUMPER_DIR}/Platform/Windows/LocalBackend.cpp)
	target_compile_definitions(DumperCore PUBLIC UNICODE _UNICODE)
	target_link_libraries(DumperCore PUBLIC version)

	add_library(UnrealMappingsDumper SHARED ${DUMPER_DIR}/dllmain.cpp)
	target_link_libraries(UnrealMappingsDumper PRIVATE DumperCore)
	set_target_properties(UnrealMappingsDumper PROPERTIES PREFIX "")
	umd_static_runtime(UnrealMappingsDumper)

	add_executable(UnrealMappingsInjector ${CMAKE_CURRENT_SOURCE_DIR}/Injector/Injector.cpp)
	target_compile_definitions(UnrealMappingsInjector PRIVATE UNICODE _UNICODE)
	umd_static_runtime(UnrealMappingsInjector)

	if(MINGW)
		target_link_options(UnrealMappingsInjector PRIVATE -municode)
	endif()

	install(TARGETS UnrealMappingsDumper UnrealMappingsInjector RUNTIME DESTINATION . LIBRARY DESTINATION .)
	install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/inject-linux.sh DESTINATION .)
else()
	target_sources(DumperCore PRIVATE
		${DUMPER_DIR}/Platform/Linux/Process.cpp
		${DUMPER_DIR}/Platform/Linux/RemoteBackend.cpp
	)

	add_executable(unreal-mappings-dumper ${DUMPER_DIR}/Platform/Linux/main.cpp)
	target_link_libraries(unreal-mappings-dumper PRIVATE DumperCore)
	umd_static_runtime(unreal-mappings-dumper)

	install(TARGETS unreal-mappings-dumper RUNTIME DESTINATION bin)
endif()

if(UMD_BUILD_TESTS)
	enable_testing()

	add_executable(UnrealMappingsDumper.Tests ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FakeEngineTest.cpp)
	target_link_libraries(UnrealMappingsDumper.Tests PRIVATE DumperCore)
	umd_static_runtime(UnrealMappingsDumper.Tests)

	foreach(layout classic newfield optimized)
		add_test(NAME FakeEngine.${layout} COMMAND UnrealMappingsDumper.Tests ${layout})
		set_tests_properties(FakeEngine.${layout} PROPERTIES RUN_SERIAL TRUE)
	endforeach()
endif()
