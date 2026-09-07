# Copyright (c)
# See the LICENSE file in the package base directory for details


set(LCOV_DATA_PATH "${CMAKE_BINARY_DIR}/lcov/data")
set(LCOV_DATA_PATH_INIT "${LCOV_DATA_PATH}/init")
set(LCOV_DATA_PATH_CAPTURE "${LCOV_DATA_PATH}/capture")
set(LCOV_HTML_PATH "${CMAKE_BINARY_DIR}/lcov/html")




find_package(Gcov)




function (add_lcov_target TNAME)
	if (LCOV_FOUND)
		lcov_capture_initial_tgt(${TNAME})

		lcov_capture_tgt(${TNAME})
	endif ()
endfunction (add_lcov_target)




include(FindPackageHandleStandardArgs)

find_program(LCOV_BIN lcov)
find_program(GENINFO_BIN geninfo)
find_program(GENHTML_BIN genhtml)
find_package_handle_standard_args(lcov
	REQUIRED_VARS LCOV_BIN GENINFO_BIN GENHTML_BIN
)

set(GENHTML_CPPFILT_FLAG "")
find_program(CPPFILT_BIN c++filt)
if (NOT CPPFILT_BIN STREQUAL "")
	set(GENHTML_CPPFILT_FLAG "--demangle-cpp")
endif (NOT CPPFILT_BIN STREQUAL "")

if (GENINFO_BIN AND NOT DEFINED GENINFO_EXTERN_FLAG)
	set(FLAG "")
	execute_process(COMMAND ${GENINFO_BIN} --help OUTPUT_VARIABLE GENINFO_HELP)
	string(REGEX MATCH "external" GENINFO_RES "${GENINFO_HELP}")
	if (GENINFO_RES)
		set(FLAG "--no-external")
	endif ()

	set(GENINFO_EXTERN_FLAG "${FLAG}"
		CACHE STRING "Geninfo flag to exclude system sources.")
endif ()

if (NOT LCOV_FOUND)
	return()
endif (NOT LCOV_FOUND)




file(MAKE_DIRECTORY ${LCOV_DATA_PATH_INIT})
file(MAKE_DIRECTORY ${LCOV_DATA_PATH_CAPTURE})

set(LCOV_REMOVE_PATTERNS "")

function (lcov_merge_files OUTFILE ...)
	list(REMOVE_AT ARGV 0)

	string(REPLACE "${CMAKE_BINARY_DIR}/" "" FILE_REL "${OUTFILE}")
	add_custom_command(OUTPUT "${OUTFILE}.raw"
		COMMAND cat ${ARGV} > ${OUTFILE}.raw
		DEPENDS ${ARGV}
		COMMENT "Generating ${FILE_REL}"
	)

	add_custom_command(OUTPUT "${OUTFILE}"
		COMMAND ${LCOV_BIN} --quiet -a ${OUTFILE}.raw --output-file ${OUTFILE}
			--base-directory ${PROJECT_SOURCE_DIR} ${LCOV_EXTRA_FLAGS}
		COMMAND ${LCOV_BIN} --quiet -r ${OUTFILE} ${LCOV_REMOVE_PATTERNS}
			--output-file ${OUTFILE} ${LCOV_EXTRA_FLAGS}
		DEPENDS ${OUTFILE}.raw
		COMMENT "Post-processing ${FILE_REL}"
	)
endfunction ()


function (lcov_collect_target_geninfo_files TNAME MODE GENINFO_FILES_VAR HAS_GCOV_VAR)
	codecov_target_gcov_settings(${TNAME} SOURCES _TCOMPILER GCOV_BIN GCOV_ENV)
	if (NOT GCOV_BIN)
		set(${GENINFO_FILES_VAR} "" PARENT_SCOPE)
		set(${HAS_GCOV_VAR} FALSE PARENT_SCOPE)
		return()
	endif ()

	set(TDIR "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TNAME}.dir")
	set(GENINFO_FILES "")
	foreach(FILE ${SOURCES})
		if ("${MODE}" STREQUAL "INITIAL")
			set(OUTFILE "${TDIR}/${FILE}.info.init")
			add_custom_command(OUTPUT ${OUTFILE} COMMAND ${GCOV_ENV} ${GENINFO_BIN}
					--quiet --base-directory ${PROJECT_SOURCE_DIR} --initial
					--gcov-tool ${GCOV_BIN} --output-filename ${OUTFILE}
					${GENINFO_EXTERN_FLAG} ${TDIR}/${FILE}.gcno
				DEPENDS ${TNAME}
				COMMENT "Capturing initial coverage data for ${FILE}"
			)
		elseif ("${MODE}" STREQUAL "CAPTURE")
			set(OUTFILE "${TDIR}/${FILE}.info")
			add_custom_command(OUTPUT ${OUTFILE}
				COMMAND test -f "${TDIR}/${FILE}.gcda"
					&& ${GCOV_ENV} ${GENINFO_BIN} --quiet --base-directory
						${PROJECT_SOURCE_DIR} --gcov-tool ${GCOV_BIN}
						--output-filename ${OUTFILE} ${GENINFO_EXTERN_FLAG}
						${TDIR}/${FILE}.gcda
					|| cp ${OUTFILE}.init ${OUTFILE}
				DEPENDS ${TNAME} ${TNAME}-capture-init
				COMMENT "Capturing coverage data for ${FILE}"
			)
		else ()
			message(FATAL_ERROR "Unknown lcov capture mode: ${MODE}")
		endif ()

		list(APPEND GENINFO_FILES ${OUTFILE})
	endforeach()

	set(${GENINFO_FILES_VAR} "${GENINFO_FILES}" PARENT_SCOPE)
	set(${HAS_GCOV_VAR} TRUE PARENT_SCOPE)
endfunction ()


function (lcov_register_capture_target OUTFILE TARGET_NAME AGGREGATE_TARGET CACHE_VAR ADD_TO_ALL INITIAL_MODE)
	if (INITIAL_MODE)
		set(LCOV_EXTRA_FLAGS "--initial")
	else ()
		set(LCOV_EXTRA_FLAGS "")
	endif ()

	lcov_merge_files("${OUTFILE}" ${ARGN})
	if (ADD_TO_ALL)
		add_custom_target(${TARGET_NAME} ALL DEPENDS ${OUTFILE})
	else ()
		add_custom_target(${TARGET_NAME} DEPENDS ${OUTFILE})
	endif ()

	add_dependencies(${AGGREGATE_TARGET} ${TARGET_NAME})
	set(${CACHE_VAR} "${${CACHE_VAR}}" "${OUTFILE}" CACHE INTERNAL "")
endfunction ()




if (NOT TARGET lcov-capture-init)
	add_custom_target(lcov-capture-init)
	set(LCOV_CAPTURE_INIT_FILES "" CACHE INTERNAL "")
endif (NOT TARGET lcov-capture-init)


function (lcov_capture_initial_tgt TNAME)
	lcov_collect_target_geninfo_files(${TNAME} INITIAL GENINFO_FILES HAS_GCOV)
	if (NOT HAS_GCOV)
		return()
	endif ()

	set(OUTFILE "${LCOV_DATA_PATH_INIT}/${TNAME}.info")
	lcov_register_capture_target("${OUTFILE}" ${TNAME}-capture-init
		lcov-capture-init LCOV_CAPTURE_INIT_FILES TRUE TRUE ${GENINFO_FILES}
	)
endfunction (lcov_capture_initial_tgt)


function (lcov_capture_initial)
	if ("${LCOV_CAPTURE_INIT_FILES}" STREQUAL "")
		return()
	endif ()

	set(OUTFILE "${LCOV_DATA_PATH_INIT}/all_targets.info")
	lcov_merge_files("${OUTFILE}" ${LCOV_CAPTURE_INIT_FILES})
	add_custom_target(lcov-geninfo-init ALL	DEPENDS ${OUTFILE}
		lcov-capture-init
	)
endfunction (lcov_capture_initial)




if (NOT TARGET lcov-capture)
	add_custom_target(lcov-capture)
	set(LCOV_CAPTURE_FILES "" CACHE INTERNAL "")
endif (NOT TARGET lcov-capture)


function (lcov_capture_tgt TNAME)
	lcov_collect_target_geninfo_files(${TNAME} CAPTURE GENINFO_FILES HAS_GCOV)
	if (NOT HAS_GCOV)
		return()
	endif ()

	set(OUTFILE "${LCOV_DATA_PATH_CAPTURE}/${TNAME}.info")
	lcov_register_capture_target("${OUTFILE}" ${TNAME}-geninfo
		lcov-capture LCOV_CAPTURE_FILES FALSE FALSE ${GENINFO_FILES}
	)

	file(MAKE_DIRECTORY ${LCOV_HTML_PATH}/${TNAME})
	add_custom_target(${TNAME}-genhtml
		COMMAND ${GENHTML_BIN} --quiet --sort --prefix ${PROJECT_SOURCE_DIR}
			--baseline-file ${LCOV_DATA_PATH_INIT}/${TNAME}.info
			--output-directory ${LCOV_HTML_PATH}/${TNAME}
			--title "${CMAKE_PROJECT_NAME} - target ${TNAME}"
			${GENHTML_CPPFILT_FLAG} ${OUTFILE}
		DEPENDS ${TNAME}-geninfo ${TNAME}-capture-init
	)
endfunction (lcov_capture_tgt)


function (lcov_capture)
	if ("${LCOV_CAPTURE_FILES}" STREQUAL "")
		return()
	endif ()

	set(OUTFILE "${LCOV_DATA_PATH_CAPTURE}/all_targets.info")
	lcov_merge_files("${OUTFILE}" ${LCOV_CAPTURE_FILES})
	add_custom_target(lcov-geninfo DEPENDS ${OUTFILE} lcov-capture)

	if (NOT TARGET lcov)
		file(MAKE_DIRECTORY ${LCOV_HTML_PATH}/all_targets)
		add_custom_target(lcov
			COMMAND ${GENHTML_BIN} --quiet --sort
				--baseline-file ${LCOV_DATA_PATH_INIT}/all_targets.info
				--output-directory ${LCOV_HTML_PATH}/all_targets
				--title "${CMAKE_PROJECT_NAME}" --prefix "${PROJECT_SOURCE_DIR}"
				${GENHTML_CPPFILT_FLAG} ${OUTFILE}
			DEPENDS lcov-geninfo-init lcov-geninfo
		)
	endif ()
endfunction (lcov_capture)




file(MAKE_DIRECTORY ${LCOV_HTML_PATH}/selected_targets)
if (NOT TARGET lcov-genhtml)
	add_custom_target(lcov-genhtml
		COMMAND ${GENHTML_BIN}
			--quiet
			--output-directory ${LCOV_HTML_PATH}/selected_targets
			--title \"${CMAKE_PROJECT_NAME} - targets  `find
				${LCOV_DATA_PATH_CAPTURE} -name \"*.info\" ! -name
				\"all_targets.info\" -exec basename {} .info \\\;`\"
			--prefix ${PROJECT_SOURCE_DIR}
			--sort
			${GENHTML_CPPFILT_FLAG}
			`find ${LCOV_DATA_PATH_CAPTURE} -name \"*.info\" ! -name
				\"all_targets.info\"`
	)
endif (NOT TARGET lcov-genhtml)
