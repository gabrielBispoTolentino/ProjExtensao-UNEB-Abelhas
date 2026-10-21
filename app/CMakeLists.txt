cmake_minimum_required(VERSION 3.16)

# App BeeDuino (até a 1.4.0, "Abelhas Petterson"): monitor de colmeias (Qt 6 / QML, Windows e Android).
project(BeeDuino VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Svg: o ícone/logo da abelha é um SVG (também instala o plugin de imagem SVG).
# Os controles são os do estilo Basic (import QtQuick.Controls.Basic nos
# .qml), com as cores de Theme.qml: iguais no Windows e no Android, e só esse
# estilo vai no pacote.
find_package(Qt6 REQUIRED COMPONENTS Network Quick QuickControls2 Sql Svg)
# Opcional: receber leituras direto da ESP32 por Bluetooth LE. Para ativar,
# instale "Qt Connectivity" (Qt Bluetooth) pelo Qt Maintenance Tool.
find_package(Qt6 OPTIONAL_COMPONENTS Bluetooth)

qt_standard_project_setup(REQUIRES 6.10)

# Android: o Qt não traz OpenSSL, mas o Supabase só responde por HTTPS.
# Baixa as bibliotecas OpenSSL prontas da KDAB e as coloca no APK.
if (ANDROID)
    include(FetchContent)
    FetchContent_Declare(
        android_openssl
        DOWNLOAD_EXTRACT_TIMESTAMP true
        URL https://github.com/KDAB/android_openssl/archive/refs/heads/master.zip
    )
    FetchContent_MakeAvailable(android_openssl)
    include(${android_openssl_SOURCE_DIR}/android_openssl.cmake)
endif()

qt_add_executable(appBeeDuino
    main.cpp
    legacydata.cpp
    legacydata.h
    appupdater.cpp
    appupdater.h
    cloudsync.cpp
    cloudsync.h
    outsideweather.cpp
    outsideweather.h
    supabaseconfig.h
    sensormonitor.cpp
    sensormonitor.h
    sensorpayload.cpp
    sensorpayload.h
    sensorreadingstore.cpp
    sensorreadingstore.h
)

if (TARGET Qt6::Bluetooth)
    target_sources(appBeeDuino PRIVATE
        blesensorclient.cpp blesensorclient.h
        boardcontrol.cpp boardcontrol.h)
    target_link_libraries(appBeeDuino PRIVATE Qt6::Bluetooth)
    target_compile_definitions(appBeeDuino PRIVATE HAVE_QT_BLUETOOTH)
else()
    message(WARNING "Qt6 Bluetooth not found - building without BLE support.")
endif()

# Versão mostrada no app e comparada com a última publicada no GitHub.
target_compile_definitions(appBeeDuino PRIVATE APP_VERSION="${PROJECT_VERSION}")

# Valores do visual (cores, tamanhos de texto), usados como Theme.xxx no QML.
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appBeeDuino
    URI BeeDuino
    QML_FILES
        Main.qml
        BoardPage.qml
        BoardPicker.qml
        WifiSetupPage.qml
        BoardNamePage.qml
        RestartBoardPage.qml
        RecordIntervalPage.qml
        DevicePage.qml
        DataCleanupPage.qml
        DataExportPage.qml
        WeatherLocationPage.qml
        SensorChart.qml
        GlyphIcon.qml
        Theme.qml
        ActionButton.qml
        ChoiceChip.qml
        AppComboBox.qml
        SectionHeading.qml
        HelpText.qml
        MessageBanner.qml
        ConfirmDialog.qml
        UpdateDialog.qml
        RangeBand.qml
    RESOURCES
        icons/bee.svg
        # Atkinson Hyperlegible (SIL OFL, fonts/OFL.txt): números bem
        # distintos, legíveis no celular ao ar livre.
        fonts/AtkinsonHyperlegible-Regular.ttf
        fonts/AtkinsonHyperlegible-Bold.ttf
        # Troca os arquivos do app no Windows depois de baixar uma versão nova.
        updater/install-windows.ps1
)

# Ícone do app (a abelha): ícone do .exe no Windows e do atalho no Android.
# icons/bee.svg e o desenho do Android vêm de icons/generate_icons.py.
if (WIN32)
    # Biblioteca própria: o windres falha com caminhos de include com espaços/
    # parênteses herdados do Qt (ex.: um Vulkan SDK em "Nova pasta (4)").
    add_library(appBeeDuinoIcon OBJECT icons/appicon.rc)
    target_link_libraries(appBeeDuino PRIVATE appBeeDuinoIcon)
endif()

# App de janela no Windows (sem console).
set_target_properties(appBeeDuino PROPERTIES WIN32_EXECUTABLE TRUE)

if (ANDROID)
    set_target_properties(appBeeDuino PROPERTIES
        # Identificador do app no Android: fica com o nome antigo, senão o
        # Android trataria a BeeDuino como outro app (sem atualização por cima).
        QT_ANDROID_PACKAGE_NAME "com.abelhaspetterson.sensormonitor"
        QT_ANDROID_APP_NAME "BeeDuino"
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE 5
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
        # android/res guarda o ícone adaptável (abelha sobre creme).
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
    )
    qt_add_android_permission(appBeeDuino NAME android.permission.INTERNET)
    qt_add_android_permission(appBeeDuino NAME android.permission.ACCESS_NETWORK_STATE)
    # Bluetooth LE: permissões antigas até o Android 11, SCAN/CONNECT no 12+.
    # A localização precisa é exigida para procurar no Android 11 e anteriores.
    qt_add_android_permission(appBeeDuino
        NAME android.permission.BLUETOOTH ATTRIBUTES maxSdkVersion 30)
    qt_add_android_permission(appBeeDuino
        NAME android.permission.BLUETOOTH_ADMIN ATTRIBUTES maxSdkVersion 30)
    qt_add_android_permission(appBeeDuino NAME android.permission.BLUETOOTH_SCAN)
    qt_add_android_permission(appBeeDuino NAME android.permission.BLUETOOTH_CONNECT)
    qt_add_android_permission(appBeeDuino
        NAME android.permission.ACCESS_FINE_LOCATION ATTRIBUTES maxSdkVersion 30)
    # Abrir o instalador do Android com o APK de uma versão nova (AppUpdater).
    qt_add_android_permission(appBeeDuino NAME android.permission.REQUEST_INSTALL_PACKAGES)
    # Segundo plano (AndroidBackground + android/src/.../MonitorService.java):
    # serviço em primeiro plano do tipo "dispositivo conectado" e a notificação
    # dele. O Android 14+ exige, para esse tipo, uma permissão da lista dele;
    # CHANGE_NETWORK_STATE é dada na instalação, sem perguntar.
    qt_add_android_permission(appBeeDuino NAME android.permission.FOREGROUND_SERVICE)
    qt_add_android_permission(appBeeDuino NAME android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE)
    qt_add_android_permission(appBeeDuino NAME android.permission.CHANGE_NETWORK_STATE)
    qt_add_android_permission(appBeeDuino NAME android.permission.POST_NOTIFICATIONS)
    target_sources(appBeeDuino PRIVATE androidbackground.cpp androidbackground.h)
    add_android_openssl_libraries(appBeeDuino)

    # Só o estilo Basic no APK: o qmlimportscanner (que diz ao androiddeployqt
    # o que copiar) passa por tools/qml_imports_basic_only.py, que tira os
    # outros estilos (o seletor de arquivos do Qt puxa todos; o app só usa o
    # Basic). Sem Python, o APK sai com todos, como antes.
    find_package(Python3 COMPONENTS Interpreter)
    unset(_scanner)
    foreach(_config RELWITHDEBINFO RELEASE MINSIZEREL DEBUG)
        get_target_property(_location Qt6::qmlimportscanner IMPORTED_LOCATION_${_config})
        if (_location AND NOT _scanner)
            set(_scanner "${_location}")
        endif()
    endforeach()
    if (Python3_Interpreter_FOUND AND _scanner AND CMAKE_HOST_WIN32)
        set(_wrapper "${CMAKE_CURRENT_BINARY_DIR}/qmlimportscanner_basic_only.bat")
        file(TO_NATIVE_PATH "${_scanner}" _scanner_native)
        file(TO_NATIVE_PATH "${Python3_EXECUTABLE}" _python_native)
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/qml_imports_basic_only.py" _filter_native)
        file(WRITE "${_wrapper}"
             "@echo off\r\n\"${_scanner_native}\" %* | \"${_python_native}\" -I \"${_filter_native}\"\r\n")
        foreach(_config RELWITHDEBINFO RELEASE MINSIZEREL DEBUG)
            set_target_properties(Qt6::qmlimportscanner PROPERTIES IMPORTED_LOCATION_${_config} "${_wrapper}")
        endforeach()
    else()
        message(STATUS "Android: all Qt Quick Controls styles will be packaged (no Python/Windows host)")
    endif()
endif()

target_link_libraries(appBeeDuino
    PRIVATE
        Qt6::Network
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Sql
        Qt6::Svg
)
