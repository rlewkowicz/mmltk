# Copyright (c)
# See the LICENSE file in the package base directory for details


include(FindPackageHandleStandardArgs)


function (codecov_collect_target_sources TNAME SOURCES_VAR TCOMPILER_VAR)
	get_target_property(TSOURCES ${TNAME} SOURCES)
	set(SOURCES "")
	set(TCOMPILER "")
	foreach (FILE ${TSOURCES})
		codecov_path_of_source(${FILE} FILE)
		if (NOT "${FILE}" STREQUAL "")
			codecov_lang_of_source(${FILE} LANG)
			if (NOT "${LANG}" STREQUAL "")
				list(APPEND SOURCES "${FILE}")
				set(TCOMPILER ${CMAKE_${LANG}_COMPILER_ID})
			endif ()
		endif ()
	endforeach ()

	set(${SOURCES_VAR} "${SOURCES}" PARENT_SCOPE)
	set(${TCOMPILER_VAR} "${TCOMPILER}" PARENT_SCOPE)
endfunction()


function (codecov_target_gcov_settings TNAME SOURCES_VAR TCOMPILER_VAR GCOV_BIN_VAR GCOV_ENV_VAR)
	codecov_collect_target_sources(${TNAME} SOURCES TCOMPILER)

	if (NOT GCOV_${TCOMPILER}_BIN)
		message(WARNING "No coverage evaluation binary found for ${TCOMPILER}.")
		set(${SOURCES_VAR} "" PARENT_SCOPE)
		set(${TCOMPILER_VAR} "" PARENT_SCOPE)
		set(${GCOV_BIN_VAR} "" PARENT_SCOPE)
		set(${GCOV_ENV_VAR} "" PARENT_SCOPE)
		return()
	endif ()

	set(${SOURCES_VAR} "${SOURCES}" PARENT_SCOPE)
	set(${TCOMPILER_VAR} "${TCOMPILER}" PARENT_SCOPE)
	set(${GCOV_BIN_VAR} "${GCOV_${TCOMPILER}_BIN}" PARENT_SCOPE)
	set(${GCOV_ENV_VAR} "${GCOV_${TCOMPILER}_ENV}" PARENT_SCOPE)
endfunction()


set(CMAKE_REQUIRED_QUIET_SAVE ${CMAKE_REQUIRED_QUIET})
set(CMAKE_REQUIRED_QUIET ${codecov_FIND_QUIETLY})

get_property(ENABLED_LANGUAGES GLOBAL PROPERTY ENABLED_LANGUAGES)
foreach (LANG ${ENABLED_LANGUAGES})
	if (NOT GCOV_${CMAKE_${LANG}_COMPILER_ID}_BIN)
		get_filename_component(COMPILER_PATH "${CMAKE_${LANG}_COMPILER}" PATH)

		if ("${CMAKE_${LANG}_COMPILER_ID}" STREQUAL "GNU")
			string(REGEX MATCH "^[0-9]+" GCC_VERSION
				"${CMAKE_${LANG}_COMPILER_VERSION}")

			find_program(GCOV_BIN NAMES gcov-${GCC_VERSION} gcov
				HINTS ${COMPILER_PATH})

		elseif ("${CMAKE_${LANG}_COMPILER_ID}" STREQUAL "Clang")
			string(REGEX MATCH "^[0-9]+.[0-9]+" LLVM_VERSION
				"${CMAKE_${LANG}_COMPILER_VERSION}")

			if(LLVM_VERSION VERSION_GREATER 3.4)
				find_program(LLVM_COV_BIN NAMES "llvm-cov-${LLVM_VERSION}"
					"llvm-cov" HINTS ${COMPILER_PATH})
				mark_as_advanced(LLVM_COV_BIN)

				if (LLVM_COV_BIN)
					find_program(LLVM_COV_WRAPPER "llvm-cov-wrapper" PATHS
						${CMAKE_MODULE_PATH})
					if (LLVM_COV_WRAPPER)
						set(GCOV_BIN "${LLVM_COV_WRAPPER}" CACHE FILEPATH "")

						set(GCOV_${CMAKE_${LANG}_COMPILER_ID}_ENV
							"LLVM_COV_BIN=${LLVM_COV_BIN}" CACHE STRING
							"Environment variables for llvm-cov-wrapper.")
						mark_as_advanced(GCOV_${CMAKE_${LANG}_COMPILER_ID}_ENV)
					endif ()
				endif ()
			endif ()

			if (NOT GCOV_BIN)
				find_program(GCOV_BIN gcov HINTS ${COMPILER_PATH})
			endif ()
		endif ()


		if (GCOV_BIN)
			set(GCOV_${CMAKE_${LANG}_COMPILER_ID}_BIN "${GCOV_BIN}" CACHE STRING
				"${LANG} gcov binary.")

			if (NOT CMAKE_REQUIRED_QUIET)
				message("-- Found gcov evaluation for "
				"${CMAKE_${LANG}_COMPILER_ID}: ${GCOV_BIN}")
			endif()

			unset(GCOV_BIN CACHE)
		endif ()
	endif ()
endforeach ()




if (NOT TARGET gcov)
	add_custom_target(gcov)
endif (NOT TARGET gcov)



function (add_gcov_target TNAME)
	set(TDIR ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TNAME}.dir)

	codecov_target_gcov_settings(${TNAME} SOURCES TCOMPILER GCOV_BIN GCOV_ENV)
	if (NOT GCOV_BIN)
		return()
	endif ()


	set(BUFFER "")
	foreach(FILE ${SOURCES})
		get_filename_component(FILE_PATH "${TDIR}/${FILE}" PATH)

		add_custom_command(OUTPUT ${TDIR}/${FILE}.gcov
			COMMAND ${GCOV_ENV} ${GCOV_BIN} ${TDIR}/${FILE}.gcno > /dev/null
			DEPENDS ${TNAME} ${TDIR}/${FILE}.gcno
			WORKING_DIRECTORY ${FILE_PATH}
		)

		list(APPEND BUFFER ${TDIR}/${FILE}.gcov)
	endforeach()


	add_custom_target(${TNAME}-gcov DEPENDS ${BUFFER})

	add_dependencies(gcov ${TNAME}-gcov)
endfunction (add_gcov_target)
