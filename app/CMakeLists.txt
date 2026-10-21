cmake_minimum_required(VERSION 3.16)

# App Abelhas Petterson: monitor de colmeias (Qt 6 / QML, Windows e Android).
project(AbelhasPetterson VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Svg: o ícone/logo da abelha é um SVG (também instala o plugin de imagem SVG).
# QuickControls2: o app usa o estilo Basic (com as cores de Theme.qml) para
# ficar igual no Windows e no Android.
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

qt_add_executable(appAbelhasPetterson
    main.cpp
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
    target_sources(appAbelhasPetterson PRIVATE
        blesensorclient.cpp blesensorclient.h
        boardcontrol.cpp boardcontrol.h)
    target_link_libraries(appAbelhasPetterson PRIVATE Qt6::Bluetooth)
    target_compile_definitions(appAbelhasPetterson PRIVATE HAVE_QT_BLUETOOTH)
else()
    message(WARNING "Qt6 Bluetooth not found - building without BLE support.")
endif()

# Versão mostrada no app e comparada com a última publicada no GitHub.
target_compile_definitions(appAbelhasPetterson PRIVATE APP_VERSION="${PROJECT_VERSION}")

# Valores do visual (cores, tamanhos de texto), usados como Theme.xxx no QML.
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appAbelhasPetterson
    URI AbelhasPetterson
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
    add_library(appAbelhasPettersonIcon OBJECT icons/appicon.rc)
    target_link_libraries(appAbelhasPetterson PRIVATE appAbelhasPettersonIcon)
endif()

# Propriedades de pacote para macOS/iOS (não usadas por este projeto).
set_target_properties(appAbelhasPetterson PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appAbelhasPetterson
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if (ANDROID)
    set_target_properties(appAbelhasPetterson PROPERTIES
        QT_ANDROID_PACKAGE_NAME "com.abelhaspetterson.sensormonitor"
        QT_ANDROID_APP_NAME "Abelhas Petterson"
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE 4
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
        # android/res guarda o ícone adaptável (abelha sobre creme).
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
    )
    qt_add_android_permission(appAbelhasPetterson NAME android.permission.INTERNET)
    qt_add_android_permission(appAbelhasPetterson NAME android.permission.ACCESS_NETWORK_STATE)
    # Bluetooth LE: permissões antigas até o Android 11, SCAN/CONNECT no 12+.
    # A localização precisa é exigida para procurar no Android 11 e anteriores.
    qt_add_android_permission(appAbelhasPetterson
        NAME android.permission.BLUETOOTH ATTRIBUTES maxSdkVersion 30)
    qt_add_android_permission(appAbelhasPetterson
        NAME android.permission.BLUETOOTH_ADMIN ATTRIBUTES maxSdkVersion 30)
    qt_add_android_permission(appAbelhasPetterson NAME android.permission.BLUETOOTH_SCAN)
    qt_add_android_permission(appAbelhasPetterson NAME android.permission.BLUETOOTH_CONNECT)
    qt_add_android_permission(appAbelhasPetterson
        NAME android.permission.ACCESS_FINE_LOCATION ATTRIBUTES maxSdkVersion 30)
    # Abrir o instalador do Android com o APK de uma versão nova (AppUpdater).
    qt_add_android_permission(appAbelhasPetterson NAME android.permission.REQUEST_INSTALL_PACKAGES)
    add_android_openssl_libraries(appAbelhasPetterson)
endif()

target_link_libraries(appAbelhasPetterson
    PRIVATE
        Qt6::Network
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Sql
        Qt6::Svg
)

include(GNUInstallDirs)
install(TARGETS appAbelhasPetterson
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
