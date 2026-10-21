# Testes do firmware no PC: o ColmeiaMonitor.ino de verdade sobre uma ESP32
# simulada (shim/), dirigido por um celular simulado (fw_sim.cpp).
#
#   cmake -S tests -B <pasta> -G Ninja
#   cmake --build <pasta>
#   ctest --test-dir <pasta> --output-on-failure
#
# Precisa do ArduinoJson (o mesmo da IDE do Arduino) e do Python, que gera o
# .cpp com os protótipos a partir do .ino (gen_sketch.py).

cmake_minimum_required(VERSION 3.16)
project(ColmeiaMonitorSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ARDUINOJSON_DIR "$ENV{USERPROFILE}/Documents/Arduino/libraries/ArduinoJson/src"
    CACHE PATH "Pasta src/ do ArduinoJson")

set(SKETCH ${CMAKE_CURRENT_SOURCE_DIR}/../ColmeiaMonitor/ColmeiaMonitor.ino)
set(SKETCH_CPP ${CMAKE_CURRENT_BINARY_DIR}/sketch.cpp)
add_custom_command(
    OUTPUT ${SKETCH_CPP}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/gen_sketch.py ${SKETCH} ${SKETCH_CPP}
    DEPENDS ${SKETCH} ${CMAKE_CURRENT_SOURCE_DIR}/gen_sketch.py
    COMMENT "Generating sketch.cpp from ColmeiaMonitor.ino")

add_executable(fw_sim fw_sim.cpp ${SKETCH_CPP})
set_source_files_properties(${SKETCH_CPP} PROPERTIES HEADER_FILE_ONLY ON)   # entra por #include no fw_sim.cpp
target_include_directories(fw_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${CMAKE_CURRENT_BINARY_DIR} ${ARDUINOJSON_DIR})
target_compile_definitions(fw_sim PRIVATE
    ARDUINO=10819
    ARDUINOJSON_ENABLE_ARDUINO_STREAM=0
    ARDUINOJSON_ENABLE_ARDUINO_PRINT=0
    ARDUINOJSON_ENABLE_PROGMEM=0)
if (MINGW)
    target_link_options(fw_sim PRIVATE -static)   # roda sem as DLLs do MinGW no PATH
endif()

enable_testing()
set(WORK ${CMAKE_CURRENT_BINARY_DIR}/work)
foreach(scenario
        offline_dump_default_mtu large_backlog_big_mtu large_backlog_default_mtu
        unacknowledged_batch_is_resent disconnect_midway_resumes
        record_written_while_waiting_for_ack wifi_first wifi_up_cloud_down
        no_wifi_retries_during_dump notify_failures_are_retried old_lines_on_the_card
        record_interval_has_no_drift rtc_dead_battery_and_ntp no_rtc)
    add_test(NAME ${scenario} COMMAND fw_sim ${scenario} ${WORK}/${scenario})
endforeach()
# Dois processos sobre a mesma pasta: o segundo é a placa reiniciada.
foreach(pair reboot undatable_after_reboot rtc)
    add_test(NAME ${pair}_part1 COMMAND fw_sim ${pair}_part1 ${WORK}/${pair})
    add_test(NAME ${pair}_part2 COMMAND fw_sim ${pair}_part2 ${WORK}/${pair})
    set_tests_properties(${pair}_part2 PROPERTIES DEPENDS ${pair}_part1)
endforeach()
