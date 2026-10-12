cmake_minimum_required(VERSION 3.20)

project(UnrealMappingsDumper VERSION 1.1.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Only 64 bit builds are supported, Unreal Engine games are 64 bit.")
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "Only Windows and Linux are supported.")
endif()

option(UMD_BUILD_TESTS "Build the fake engine tests" ON)
option(UMD_BUILD_GUI "Build the window for the dumper" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DUMPER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/UnrealMappingsDumper)

add_library(DumperCore STATIC
	${DUMPER_DIR}/aes.cpp
	${DUMPER_DIR}/aeskey.cpp
	${DUMPER_DIR}/app.cpp
	${DUMPER_DIR}/cli.cpp
	${DUMPER_DIR}/dumper.cpp
	${DUMPER_DIR}/log.cpp
	${DUMPER_DIR}/memory.cpp
	${DUMPER_DIR}/names.cpp
	${DUMPER_DIR}/process.cpp
	${DUMPER_DIR}/remote.cpp
	${DUMPER_DIR}/scanning.cpp
	${DUMPER_DIR}/unrealVersion.cpp
)

target_compile_definitions(DumperCore PUBLIC UMD_VERSION="${PROJECT_VERSION}")

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
	target_sources(DumperCore PRIVATE
		${DUMPER_DIR}/Platform/Windows/LocalBackend.cpp
		${DUMPER_DIR}/Platform/Windows/Process.cpp
		${DUMPER_DIR}/Platform/Windows/RemoteBackend.cpp
	)
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

if(UMD_BUILD_GUI)
	include(FetchContent)
	include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake)

	FetchContent_Declare(imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG v1.91.8
		GIT_SHALLOW TRUE
	)
	FetchContent_MakeAvailable(imgui)

	umd_embed_files(${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp
		UmdFontRoboto ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
		UmdFontCousine ${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf
	)

	add_library(DumperGui STATIC
		${DUMPER_DIR}/Gui/App.cpp
		${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp
		${imgui_SOURCE_DIR}/imgui.cpp
		${imgui_SOURCE_DIR}/imgui_draw.cpp
		${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp
	)

	target_include_directories(DumperGui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
	target_link_libraries(DumperGui PUBLIC DumperCore)

	if(WIN32)
		target_sources(DumperGui PRIVATE
			${DUMPER_DIR}/Gui/WindowWin32.cpp
			${DUMPER_DIR}/Gui/SystemWindows.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
		)
		target_link_libraries(DumperGui PUBLIC d3d11 d3dcompiler dwmapi imm32)

		add_executable(UnrealMappingsDumperApp WIN32 ${DUMPER_DIR}/Platform/Windows/AppMain.cpp)
		target_link_libraries(UnrealMappingsDumperApp PRIVATE DumperGui)
		set_target_properties(UnrealMappingsDumperApp PROPERTIES OUTPUT_NAME UnrealMappingsDumper PDB_NAME UnrealMappingsDumperApp)
		umd_static_runtime(UnrealMappingsDumperApp)

		if(MINGW)
			target_link_options(UnrealMappingsDumperApp PRIVATE -municode)
		endif()

		if(MSVC)
			set_property(TARGET DumperGui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()

		install(TARGETS UnrealMappingsDumperApp RUNTIME DESTINATION .)
	else()
		set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
		set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)

		FetchContent_Declare(glfw
			GIT_REPOSITORY https://github.com/glfw/glfw.git
			GIT_TAG 3.4
			GIT_SHALLOW TRUE
		)
		FetchContent_MakeAvailable(glfw)

		target_sources(DumperGui PRIVATE
			${DUMPER_DIR}/Gui/WindowGlfw.cpp
			${DUMPER_DIR}/Gui/SystemLinux.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
			${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
		)
		target_link_libraries(DumperGui PUBLIC glfw ${CMAKE_DL_LIBS})
		target_compile_definitions(DumperGui PRIVATE GLFW_INCLUDE_NONE)

		target_link_libraries(unreal-mappings-dumper PRIVATE DumperGui)
		target_compile_definitions(unreal-mappings-dumper PRIVATE UMD_WITH_GUI)
	endif()
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

	add_test(NAME FakeEngine.aes COMMAND UnrealMappingsDumper.Tests aes)
	set_tests_properties(FakeEngine.aes PROPERTIES RUN_SERIAL TRUE)
endif()
