##==============================================================================
## General.
##==============================================================================
cmake_minimum_required(VERSION 3.19)
project(ArtistToolProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_EXTENSIONS FALSE)

set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_SKIP_BUILD_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "$ORIGIN")

set(CMAKE_AUTOMOC TRUE)

##==============================================================================
## Variables.
##==============================================================================
# Naming convention for editor plugins (UNIGINE Store requirement):
#   [lib]<VENDOR_NAME><target>_editorplugin[_double]_x64[d].dll/.so
# The "d" (debug) binary is a RelWithDebInfo build: optimized, /MD, with debug
# info, linked against the d-suffixed engine/editor libraries.
option(UNIGINE_DOUBLE "Build against the double precision engine/editor libraries" OFF)
# The Debug tab can show two reference pages for developing the plugin: a
# gallery of UI controls and a browser of the editor's built-in icons. They are
# not a part of a release.
option(ARTIST_TOOL_DEV_PAGES "Build the reference pages of the Debug tab" OFF)
set(vendor_name zloy_pingvin)
set(target artist_tool)

set(binary_name ${vendor_name}${target}_editorplugin)
if (UNIGINE_DOUBLE)
	string(APPEND binary_name "_double")
endif()
string(APPEND binary_name "_x64")

##==============================================================================
## Environment.
##==============================================================================
# Root of the UNIGINE project/SDK providing include/, lib/ and bin/. Defaults
# to the standard in-project location (<project>/source/plugins/<vendor>/<name>).
set(UNIGINE_SDK_PATH "${PROJECT_SOURCE_DIR}/../../../../" CACHE PATH
	"Path to the UNIGINE project or SDK root (must contain include/, lib/, bin/)")
set(UNIGINE_BIN_DIR ${UNIGINE_SDK_PATH}/bin/)
set(UNIGINE_LIB_DIR ${UNIGINE_SDK_PATH}/lib/)
set(UNIGINE_INCLUDE_DIR ${UNIGINE_SDK_PATH}/include/)
# Where the plugin binary is written. Defaults to the plugins folder of the
# project/SDK, from which the editor loads it.
set(ARTIST_TOOL_OUTPUT_DIR "${UNIGINE_BIN_DIR}plugins/${vendor_name}/${target}" CACHE PATH
	"Folder the plugin binary is written to")
set(OUTPUT_PLUGIN_DIR ${ARTIST_TOOL_OUTPUT_DIR})

##==============================================================================
## Dependencies.
##==============================================================================
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
find_package(Engine REQUIRED MODULE QUIET)
find_package(Editor REQUIRED MODULE QUIET)

# UNIGINE_QTROOT points to the Qt distribution used by the Editor (6.5.3).
set(QT_ROOT "$ENV{UNIGINE_QTROOT}")
if (QT_ROOT AND EXISTS "${QT_ROOT}")
	list(APPEND CMAKE_PREFIX_PATH ${QT_ROOT})
endif()

# Editor's Debug build uses Release binaries of the Qt framework.
set(temp_map_imported ${CMAKE_MAP_IMPORTED_CONFIG_DEBUG})
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG RELEASE)

find_package(Qt6 6.5.3 CONFIG EXACT COMPONENTS Core Gui Widgets)

set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG ${temp_map_imported})
unset(temp_map_imported)

if (NOT Qt6_FOUND)
	message(FATAL_ERROR "Qt 6.5.3 is not found. Install it and pass "
	"-DCMAKE_PREFIX_PATH=<path-to-Qt>/6.5.3/<msvc2019_64 or gcc_64> or set the "
	"UNIGINE_QTROOT environment variable.")
endif()

##==============================================================================
## Target.
##==============================================================================
add_library(${target} SHARED
	${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/${target}.h
	${CMAKE_CURRENT_SOURCE_DIR}/ArtistToolWindow.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ArtistToolWindow.h
	${CMAKE_CURRENT_SOURCE_DIR}/ClutterUnpacker.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ClutterUnpacker.h
	${CMAKE_CURRENT_SOURCE_DIR}/ClutterUnpackPage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ClutterUnpackPage.h
	${CMAKE_CURRENT_SOURCE_DIR}/Localization.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Localization.h
	${CMAKE_CURRENT_SOURCE_DIR}/ObjectPlacer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ObjectPlacer.h
	${CMAKE_CURRENT_SOURCE_DIR}/ObjectPlacerPage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ObjectPlacerPage.h
	${CMAKE_CURRENT_SOURCE_DIR}/PathPlacer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/PathPlacer.h
	${CMAKE_CURRENT_SOURCE_DIR}/PathPlacerPage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/PathPlacerPage.h
	${CMAKE_CURRENT_SOURCE_DIR}/PlaceableAsset.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/PlaceableAsset.h
	${CMAKE_CURRENT_SOURCE_DIR}/UiHelpers.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/UiHelpers.h
	${CMAKE_CURRENT_SOURCE_DIR}/ViewportPicking.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ViewportPicking.h
	)

if (ARTIST_TOOL_DEV_PAGES)
	target_sources(${target} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/IconBrowserPage.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/IconBrowserPage.h
		${CMAKE_CURRENT_SOURCE_DIR}/UiGalleryPage.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/UiGalleryPage.h
		)
endif()

if(UNIX)
	set_target_properties(${target} PROPERTIES
		C_VISIBILITY_PRESET hidden
		CXX_VISIBILITY_PRESET hidden
	)
endif()

set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_include_directories(${target} SYSTEM
	PRIVATE
	${UNIGINE_INCLUDE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	)

target_link_libraries(${target}
	PRIVATE
	Unigine::Engine
	Unigine::Editor
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
	)

##==============================================================================
## Target compiling options and properties.
##==============================================================================
target_compile_definitions(${target}
	PRIVATE
	$<$<BOOL:${UNIX}>:_LINUX>
	$<$<BOOL:${UNIGINE_DOUBLE}>:UNIGINE_DOUBLE>
	$<$<BOOL:${ARTIST_TOOL_DEV_PAGES}>:ARTIST_TOOL_DEV_PAGES>
	QT_NO_DEBUG
	)

## the plugin manifest is embedded by moc: reconfigure when it changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/artist_tool.json")

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	set(UNIGINE_COMPILER_IS_MSVC TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	set(UNIGINE_COMPILER_IS_CLANG TRUE)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(UNIGINE_COMPILER_IS_GNU TRUE)
endif()

if (UNIGINE_COMPILER_IS_MSVC)
	target_compile_definitions(${target}
		PRIVATE
		_CRT_SECURE_NO_DEPRECATE
	)
	include(ProcessorCount)
	ProcessorCount(proc_count)
	target_compile_options(${target}
		PRIVATE
		/TP				# Specifies all source files are C++.
		/FS				# Forces writes to the program database (PDB) file to be serialized through MSPDBSRV.EXE.
		/fp:fast		# Specifies floating-point behavior.
		/bigobj			# Increases the number of addressable sections in an .obj file.
		/EHsc			# Specifies the kind of exception handling used by the compiler.
		/MP${proc_count}	# Build with Multiple Processes.
		/utf-8			# Source files are UTF-8 encoded (Russian UI strings).
		/Zi				# Debug info in all configs (store requirement: PDBs for release too).
		/FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_stdext_shim.h	# Qt 6.5.3 vs new MSVC STL (stdext removed).
	)
	unset(proc_count)
	target_link_options(${target} PRIVATE "/DEBUG" "/INCREMENTAL:NO")
	target_link_options(${target} INTERFACE "/FIXED:NO")

elseif(UNIGINE_COMPILER_IS_GNU OR UNIGINE_COMPILER_IS_CLANG)

	target_compile_options(${target}
		PRIVATE
		-m64
		-msse4.2
		-ffast-math
		-fno-strict-aliasing
		$<$<NOT:$<CONFIG:Debug>>:-fomit-frame-pointer>
	)
endif ()

# Debug and RelWithDebInfo both produce the d-suffixed binary linked against
# the d-suffixed engine/editor libraries (see Find*.cmake); the store expects
# "debug" plugin binaries to be optimized builds with debug info.
set_target_properties(${target}
	PROPERTIES
	OUTPUT_NAME ${binary_name}$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:d>
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_PLUGIN_DIR}
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_PLUGIN_DIR}
	LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUTPUT_PLUGIN_DIR}
	RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_PLUGIN_DIR}
	RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_PLUGIN_DIR}
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUTPUT_PLUGIN_DIR}
	PDB_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_PLUGIN_DIR}
	PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUTPUT_PLUGIN_DIR}
	CXX_VISIBILITY_PRESET hidden
	)
