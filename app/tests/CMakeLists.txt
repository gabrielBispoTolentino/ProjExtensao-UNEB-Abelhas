# Testes automáticos do app BeeDuino (Qt Test). Compilam os mesmos arquivos
# do app, mas nunca tocam no banco real (cada teste usa uma pasta temporária)
# nem no Supabase de verdade (um servidor HTTP falso, em 127.0.0.1).
#
#   cmake -S tests -B <pasta> -G Ninja -DCMAKE_PREFIX_PATH=<Qt>/mingw_64
#   cmake --build <pasta>
#   ctest --test-dir <pasta> --output-on-failure
#
# tst_qml carrega as telas a partir do módulo QML compilado pelo app: passe
# -DBEEDUINO_APP_BUILD_DIR=<pasta de build do app> (sem ela, o teste é pulado).

cmake_minimum_required(VERSION 3.16)
project(BeeDuinoTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Network Quick QuickControls2 Sql Svg Test)
find_package(Qt6 OPTIONAL_COMPONENTS Bluetooth)
qt_standard_project_setup(REQUIRES 6.10)
enable_testing()

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BEEDUINO_APP_BUILD_DIR "" CACHE PATH "Pasta de build do app (para o tst_qml)")

# O CloudSync é compilado a partir de uma cópia, ao lado de um
# supabaseconfig.h que aponta para o servidor falso: o #include
# "supabaseconfig.h" procura primeiro na pasta do próprio .cpp, então só
# copiando ele deixa de achar o arquivo de verdade.
set(FAKE_CLOUD_DIR ${CMAKE_CURRENT_BINARY_DIR}/fakecloud)
configure_file(${APP_DIR}/cloudsync.cpp ${FAKE_CLOUD_DIR}/cloudsync.cpp COPYONLY)
configure_file(supabaseconfig_test.h ${FAKE_CLOUD_DIR}/supabaseconfig.h COPYONLY)

# ── Código do app sob teste ─────────────────────────────────────────────────
add_library(beeduino_core STATIC
    ${APP_DIR}/appupdater.cpp
    ${APP_DIR}/appupdater.h
    ${APP_DIR}/cloudsync.h
    ${FAKE_CLOUD_DIR}/cloudsync.cpp
    ${APP_DIR}/legacydata.cpp
    ${APP_DIR}/legacydata.h
    ${APP_DIR}/outsideweather.cpp
    ${APP_DIR}/outsideweather.h
    ${APP_DIR}/sensormonitor.cpp
    ${APP_DIR}/sensormonitor.h
    ${APP_DIR}/sensorpayload.cpp
    ${APP_DIR}/sensorpayload.h
    ${APP_DIR}/sensorreadingstore.cpp
    ${APP_DIR}/sensorreadingstore.h
)
target_include_directories(beeduino_core PUBLIC ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(beeduino_core PUBLIC APP_VERSION="1.4.0")
target_link_libraries(beeduino_core PUBLIC Qt6::Gui Qt6::Network Qt6::Sql)
if (TARGET Qt6::Bluetooth)
    target_sources(beeduino_core PRIVATE ${APP_DIR}/blesensorclient.cpp ${APP_DIR}/blesensorclient.h)
    target_compile_definitions(beeduino_core PUBLIC HAVE_QT_BLUETOOTH)
    target_link_libraries(beeduino_core PUBLIC Qt6::Bluetooth)
endif()

# Pasta das DLLs do Qt, para os testes rodarem pelo ctest sem mexer no PATH.
get_target_property(QT_QMAKE_LOCATION Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_LOCATION}" DIRECTORY)

function(beeduino_test name)
    qt_add_executable(${name} ${name}.cpp fakehttpserver.h testutil.h)
    target_link_libraries(${name} PRIVATE beeduino_core Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${QT_BIN_DIR}\;$ENV{PATH}"
        TIMEOUT 120)
endfunction()

beeduino_test(tst_payload)
beeduino_test(tst_store)
beeduino_test(tst_cloudsync)
beeduino_test(tst_monitor)
beeduino_test(tst_updater)
beeduino_test(tst_legacydata)
beeduino_test(tst_qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
target_compile_definitions(tst_qml PRIVATE BEEDUINO_APP_BUILD_DIR="${BEEDUINO_APP_BUILD_DIR}")
