cmake_minimum_required(VERSION 3.20)

get_filename_component(_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_windows_build_dir "${_script_dir}/.." ABSOLUTE)
get_filename_component(_default_source_root "${_windows_build_dir}/.." ABSOLUTE)
get_filename_component(_default_workspace_root "${_default_source_root}/../windows_build" ABSOLUTE)

function(mengshee_default_path variable default_value)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        set(${variable} "${default_value}" PARENT_SCOPE)
    else()
        get_filename_component(_absolute_value "${${variable}}" ABSOLUTE)
        set(${variable} "${_absolute_value}" PARENT_SCOPE)
    endif()
endfunction()

function(mengshee_find_qt_prefix out_var)
    set(_candidates)
    if(DEFINED QT_PREFIX AND NOT "${QT_PREFIX}" STREQUAL "")
        list(APPEND _candidates "${QT_PREFIX}")
    endif()
    if(DEFINED ENV{QTDIR})
        list(APPEND _candidates "$ENV{QTDIR}")
    endif()
    file(GLOB _qt_roots LIST_DIRECTORIES true "C:/Qt/*/msvc2022_64")
    list(APPEND _candidates ${_qt_roots})
    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}/lib/cmake/Qt6/Qt6Config.cmake")
            get_filename_component(_qt "${_candidate}" ABSOLUTE)
            set(${out_var} "${_qt}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Cannot find a Qt MSVC prefix. Pass -DQT_PREFIX=C:/Qt/<version>/msvc2022_64.")
endfunction()

function(mengshee_find_program out_var)
    foreach(_candidate IN LISTS ARGN)
        if(EXISTS "${_candidate}")
            get_filename_component(_program "${_candidate}" ABSOLUTE)
            set(${out_var} "${_program}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    find_program(_program_from_path NAMES "${out_var}")
    if(_program_from_path)
        set(${out_var} "${_program_from_path}" PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR "Cannot find ${out_var}.")
endfunction()

function(mengshee_find_qscintilla_root out_var)
    set(_candidates)
    if(DEFINED QSCINTILLA_ROOT AND NOT "${QSCINTILLA_ROOT}" STREQUAL "")
        list(APPEND _candidates "${QSCINTILLA_ROOT}")
    endif()
    if(DEFINED ENV{MENGSHEE_QSCINTILLA_ROOT})
        list(APPEND _candidates "$ENV{MENGSHEE_QSCINTILLA_ROOT}")
    endif()
    if(DEFINED ENV{MENGSHEE_STEMTEX_SOURCE_ROOT})
        list(APPEND _candidates "$ENV{MENGSHEE_STEMTEX_SOURCE_ROOT}/third_party")
    endif()
    if(DEFINED STEMTEX_ROOT AND NOT "${STEMTEX_ROOT}" STREQUAL "")
        list(APPEND _candidates "${STEMTEX_ROOT}/third_party")
    endif()
    list(APPEND _candidates "${SOURCE_ROOT}/external/stemtex/third_party")
    get_filename_component(_documents_root "${SOURCE_ROOT}/../.." ABSOLUTE)
    list(APPEND _candidates "${_documents_root}/xetex/stemtex/third_party")

    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}/qscintilla-src/src/Qsci/qsciscintilla.h"
            AND EXISTS "${_candidate}/qscintilla-build/release/qscintilla2_qt6.lib"
            AND EXISTS "${_candidate}/qscintilla-build/release/qscintilla2_qt6.dll")
            get_filename_component(_qscintilla "${_candidate}" ABSOLUTE)
            set(${out_var} "${_qscintilla}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Cannot find StemTeX QScintilla build. Pass -DQSCINTILLA_ROOT=<path>.")
endfunction()

function(mengshee_find_msgfmt out_var sdk_prefix)
    set(_msgfmt "${sdk_prefix}/tools/gettext-native/bin/msgfmt.exe")
    if(NOT EXISTS "${_msgfmt}")
        message(FATAL_ERROR "Cannot find native Windows msgfmt at ${_msgfmt}. Run install-gettext-native-sdk first.")
    endif()
    execute_process(COMMAND "${_msgfmt}" --version OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0 OR NOT "${_out}${_err}" MATCHES "GNU gettext-tools")
        message(FATAL_ERROR "Configured msgfmt is not GNU gettext-tools: ${_msgfmt}")
    endif()
    set(${out_var} "${_msgfmt}" PARENT_SCOPE)
endfunction()

function(mengshee_remove_inside path allowed_root)
    get_filename_component(_path "${path}" ABSOLUTE)
    get_filename_component(_allowed "${allowed_root}" ABSOLUTE)
    string(FIND "${_path}" "${_allowed}" _position)
    if(NOT _position EQUAL 0)
        message(FATAL_ERROR "Refusing to remove path outside ${_allowed}: ${_path}")
    endif()
    if(EXISTS "${_path}")
        file(REMOVE_RECURSE "${_path}")
    endif()
endfunction()

function(mengshee_remove_empty_gettext_catalogs root)
    if(NOT IS_DIRECTORY "${root}")
        return()
    endif()
    file(GLOB_RECURSE _catalogs "${root}/*.mo")
    foreach(_catalog IN LISTS _catalogs)
        file(SIZE "${_catalog}" _size)
        if(_size LESS_EQUAL 28)
            file(REMOVE "${_catalog}")
        endif()
    endforeach()
endfunction()

function(mengshee_run_vs command_line)
    set(_qt_bin "${QT_PREFIX}/bin")
    set(_sdk_bin "${SDK_PREFIX}/bin")
    file(MAKE_DIRECTORY "${WORKSPACE_ROOT}/tmp")
    set(_cmd_file "${WORKSPACE_ROOT}/tmp/mengshee-cmake-vs-env.cmd")
    file(WRITE "${_cmd_file}"
"@echo off
set \"PATH=${_qt_bin};${_sdk_bin};%PATH%\"
call \"${VCVARS}\" >nul
if errorlevel 1 exit /b %errorlevel%
${command_line}
")
    message(STATUS ">>> ${command_line}")
    execute_process(
        COMMAND cmd.exe /d /c "${_cmd_file}"
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Command failed with exit code ${_result}: ${command_line}")
    endif()
endfunction()

mengshee_default_path(SOURCE_ROOT "${_default_source_root}")
mengshee_default_path(WORKSPACE_ROOT "${_default_workspace_root}")
mengshee_default_path(SDK_PREFIX "${WORKSPACE_ROOT}/sdk")
mengshee_default_path(INSTALL_PREFIX "${WORKSPACE_ROOT}/install/mengshee")
mengshee_default_path(BUILD_DIR "${WORKSPACE_ROOT}/build/mengshee-standalone")
mengshee_find_qt_prefix(QT_PREFIX_RESOLVED)
set(QT_PREFIX "${QT_PREFIX_RESOLVED}")

if(NOT DEFINED BUILD_TYPE OR "${BUILD_TYPE}" STREQUAL "")
    set(BUILD_TYPE "RelWithDebInfo")
endif()
if(NOT DEFINED JOBS OR "${JOBS}" STREQUAL "")
    set(JOBS 8)
endif()
if(NOT DEFINED VCVARS OR "${VCVARS}" STREQUAL "")
    set(VCVARS "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat")
endif()
if(NOT EXISTS "${VCVARS}")
    message(FATAL_ERROR "Cannot find vcvars64.bat: ${VCVARS}")
endif()

if(DEFINED CMAKE_EXE AND NOT "${CMAKE_EXE}" STREQUAL "")
    set(CMAKE_PROGRAM "${CMAKE_EXE}")
else()
    set(CMAKE_PROGRAM "${CMAKE_COMMAND}")
endif()

if(DEFINED NINJA AND NOT "${NINJA}" STREQUAL "")
    set(NINJA_PROGRAM "${NINJA}")
else()
    mengshee_find_program(NINJA_PROGRAM
        "${QT_PREFIX}/../../Tools/Ninja/ninja.exe"
        "C:/Program Files/Ninja/ninja.exe"
    )
endif()
mengshee_find_qscintilla_root(QSCINTILLA_ROOT_RESOLVED)
mengshee_find_msgfmt(MSGFMT "${SDK_PREFIX}")

foreach(_required IN ITEMS
    "share/ECM/cmake/ECMConfig.cmake"
    "lib/cmake/KF6Parts/KF6PartsConfig.cmake"
    "lib/poppler.lib"
    "lib/poppler-qt6.lib"
)
    if(NOT EXISTS "${SDK_PREFIX}/${_required}")
        message(FATAL_ERROR "Standalone SDK is missing ${_required} in ${SDK_PREFIX}.")
    endif()
endforeach()

message(STATUS "Mengshee Windows CMake driver")
message(STATUS "  Source      : ${SOURCE_ROOT}")
message(STATUS "  Workspace   : ${WORKSPACE_ROOT}")
message(STATUS "  QtPrefix    : ${QT_PREFIX}")
message(STATUS "  SdkPrefix   : ${SDK_PREFIX}")
message(STATUS "  BuildDir    : ${BUILD_DIR}")
message(STATUS "  Install     : ${INSTALL_PREFIX}")
message(STATUS "  QScintilla  : ${QSCINTILLA_ROOT_RESOLVED}")

if(NOT DEFINED SKIP_BUILD OR NOT SKIP_BUILD)
    if(DEFINED CLEAN_BUILD AND CLEAN_BUILD)
        get_filename_component(_build_root "${WORKSPACE_ROOT}/build" ABSOLUTE)
        mengshee_remove_inside("${BUILD_DIR}" "${_build_root}")
    endif()
    file(MAKE_DIRECTORY "${BUILD_DIR}")

    set(_cmake_prefix_path "${QT_PREFIX}\\;${SDK_PREFIX}")
    set(_force_not_required "KF6Purpose\\;Qt6TextToSpeech\\;Phonon4Qt6\\;Freetype\\;TIFF\\;LibSpectre\\;KExiv2Qt6\\;DjVuLibre\\;EPub\\;Discount")
    set(_configure
        "\"${CMAKE_PROGRAM}\""
        -S "\"${SOURCE_ROOT}\""
        -B "\"${BUILD_DIR}\""
        -G Ninja
        "-DCMAKE_MAKE_PROGRAM=\"${NINJA_PROGRAM}\""
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
        "-DCMAKE_INSTALL_PREFIX=\"${INSTALL_PREFIX}\""
        "-DCMAKE_PREFIX_PATH=\"${_cmake_prefix_path}\""
        "-DGETTEXT_MSGFMT_EXECUTABLE=\"${MSGFMT}\""
        "-DMENGSHEE_QSCINTILLA_ROOT=\"${QSCINTILLA_ROOT_RESOLVED}\""
        -DBUILD_TESTING=OFF
        -DOKULAR_PDF_ONLY=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON
        -DKDE_INSTALL_PLUGINDIR=bin/plugins
        "-DFORCE_NOT_REQUIRED_DEPENDENCIES=\"${_force_not_required}\""
    )
    list(JOIN _configure " " _configure_command)
    mengshee_run_vs("${_configure_command}")
    mengshee_remove_empty_gettext_catalogs("${BUILD_DIR}/locale")
    mengshee_remove_empty_gettext_catalogs("${INSTALL_PREFIX}/bin/data/locale")

    set(_build_command "\"${CMAKE_PROGRAM}\" --build \"${BUILD_DIR}\" --target install --parallel ${JOBS}")
    mengshee_run_vs("${_build_command}")

    if(IS_DIRECTORY "${INSTALL_PREFIX}/plugins")
        file(MAKE_DIRECTORY "${INSTALL_PREFIX}/bin/plugins")
        file(COPY "${INSTALL_PREFIX}/plugins/" DESTINATION "${INSTALL_PREFIX}/bin/plugins")
        mengshee_remove_inside("${INSTALL_PREFIX}/plugins" "${INSTALL_PREFIX}")
        mengshee_remove_inside("${INSTALL_PREFIX}/lib/plugins" "${INSTALL_PREFIX}")
    endif()
    file(COPY_FILE
        "${QSCINTILLA_ROOT_RESOLVED}/qscintilla-build/release/qscintilla2_qt6.dll"
        "${INSTALL_PREFIX}/bin/qscintilla2_qt6.dll"
        ONLY_IF_DIFFERENT
    )
    mengshee_remove_inside("${INSTALL_PREFIX}/bin/data/mengshee/microtex" "${INSTALL_PREFIX}")
    file(REMOVE "${INSTALL_PREFIX}/bin/LaTeX.dll" "${INSTALL_PREFIX}/lib/LaTeX.lib")
endif()

if(NOT DEFINED SKIP_DEPLOY OR NOT SKIP_DEPLOY)
    set(_deploy_args
        "${CMAKE_PROGRAM}"
        "-DSOURCE_ROOT=${SOURCE_ROOT}"
        "-DWORKSPACE_ROOT=${WORKSPACE_ROOT}"
        "-DSDK_PREFIX=${SDK_PREFIX}"
        "-DINSTALL_PREFIX=${INSTALL_PREFIX}"
        "-DQT_PREFIX=${QT_PREFIX}"
        "-DQSCINTILLA_ROOT=${QSCINTILLA_ROOT_RESOLVED}"
    )
    if(DEFINED STEMTEX_ROOT AND NOT "${STEMTEX_ROOT}" STREQUAL "")
        list(APPEND _deploy_args "-DSTEMTEX_ROOT=${STEMTEX_ROOT}")
    endif()
    if(DEFINED STEMTEX_STAGE_ROOT AND NOT "${STEMTEX_STAGE_ROOT}" STREQUAL "")
        list(APPEND _deploy_args "-DSTEMTEX_STAGE_ROOT=${STEMTEX_STAGE_ROOT}")
    endif()
    list(APPEND _deploy_args -P "${_script_dir}/deploy-runtime.cmake")
    execute_process(COMMAND ${_deploy_args} RESULT_VARIABLE _deploy_result)
    if(NOT _deploy_result EQUAL 0)
        message(FATAL_ERROR "Runtime deployment failed with exit code ${_deploy_result}")
    endif()
endif()

if(NOT DEFINED SKIP_PACKAGE OR NOT SKIP_PACKAGE)
    set(_package_args
        "${CMAKE_PROGRAM}"
        "-DSOURCE_ROOT=${SOURCE_ROOT}"
        "-DWORKSPACE_ROOT=${WORKSPACE_ROOT}"
        "-DINSTALL_PREFIX=${INSTALL_PREFIX}"
    )
    if(DEFINED OUTPUT_DIR AND NOT "${OUTPUT_DIR}" STREQUAL "")
        list(APPEND _package_args "-DOUTPUT_DIR=${OUTPUT_DIR}")
    endif()
    if(DEFINED STAGE_ROOT AND NOT "${STAGE_ROOT}" STREQUAL "")
        list(APPEND _package_args "-DSTAGE_ROOT=${STAGE_ROOT}")
    endif()
    if(DEFINED CLEAN_STAGE AND CLEAN_STAGE)
        list(APPEND _package_args -DCLEAN_STAGE=ON)
    endif()
    if(DEFINED SKIP_INSTALLER AND SKIP_INSTALLER)
        list(APPEND _package_args -DSKIP_INSTALLER=ON)
    endif()
    if(DEFINED ISCC AND NOT "${ISCC}" STREQUAL "")
        list(APPEND _package_args "-DISCC=${ISCC}")
    endif()
    if(DEFINED VERSION AND NOT "${VERSION}" STREQUAL "")
        list(APPEND _package_args "-DVERSION=${VERSION}")
    endif()
    if(DEFINED FILE_VERSION AND NOT "${FILE_VERSION}" STREQUAL "")
        list(APPEND _package_args "-DFILE_VERSION=${FILE_VERSION}")
    endif()
    list(APPEND _package_args -P "${_script_dir}/package-windows.cmake")
    execute_process(COMMAND ${_package_args} RESULT_VARIABLE _package_result)
    if(NOT _package_result EQUAL 0)
        message(FATAL_ERROR "Packaging failed with exit code ${_package_result}")
    endif()
endif()

message(STATUS "Mengshee Windows CMake driver complete.")
