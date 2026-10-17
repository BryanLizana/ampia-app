cmake_minimum_required(VERSION 3.22)
project(AmpBox VERSION 0.1.0)

# JUCE: la carpeta JUCE/ si existe (clon local); si no, se descarga la misma versión.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(AmpBox
    PRODUCT_NAME "AmpBox"
    COMPANY_NAME "AmpBox"
    ICON_BIG resources/icon.png           # dibujados por tools/icono.py
    ICON_SMALL resources/icon-small.png)

target_sources(AmpBox PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/AmpliTubePreset.cpp
    Source/PresetLibrary.cpp
    Source/Looper.cpp
    Source/DrumKit.cpp
    Source/PcAudio.cpp
    Source/VoiceChain.cpp
    Source/VoiceTracks.cpp
    Source/Tuner.cpp
    Source/Crash.cpp
    Source/Theme.cpp
    Source/MainComponent.cpp)

target_compile_definitions(AmpBox PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AmpBox,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AmpBox,JUCE_VERSION>")

# SDKs de Steinberg: no van en el repo (licencia de Steinberg). Si están, se activan:
#   asiosdk/  -> ASIO (latencia baja con la interfaz). Sin él, solo audio de Windows.
#   vst2sdk/  -> plugins VST2 (Guitar Rig 5). Sin él, solo VST3.
# asiosdk/: copiar common/*.h del ASIO SDK (steinberg.net). vst2sdk/: pluginterfaces/vst2.x/aeffect*.h de un VST SDK 3.6.x.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/iasiodrv.h")
    target_compile_definitions(AmpBox PRIVATE JUCE_ASIO=1)
    target_include_directories(AmpBox PRIVATE asiosdk)
    message(STATUS "AmpBox: ASIO activado")
else()
    message(WARNING "AmpBox: falta asiosdk/ -> sin ASIO (solo audio de Windows). Copiar los .h de common/ del ASIO SDK de Steinberg en asiosdk/")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst2sdk/pluginterfaces/vst2.x/aeffect.h")
    target_compile_definitions(AmpBox PRIVATE JUCE_PLUGINHOST_VST=1)
    target_include_directories(AmpBox PRIVATE vst2sdk)
    message(STATUS "AmpBox: VST2 activado")
else()
    message(STATUS "AmpBox: sin vst2sdk/ -> solo plugins VST3")
endif()

# Símbolos (.pdb) también en Release: si se cae, la pila del log dice dónde.
if(MSVC)
    target_compile_options(AmpBox PRIVATE /Zi)
    target_link_options(AmpBox PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# Fuentes incrustadas (licencia OFL): Inter para la interfaz, Source Serif 4 para títulos.
juce_add_binary_data(AmpBoxFonts SOURCES
    resources/fonts/Inter-400.ttf
    resources/fonts/Inter-600.ttf
    resources/fonts/SourceSerif4-600.ttf)

target_link_libraries(AmpBox
    PRIVATE
        AmpBoxFonts
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Si cambia icon.ico (lo arma JUCE al configurar), volver a compilar el recurso de Windows
# (si no, el .exe queda con el ícono viejo).
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/AmpBox_artefacts/JuceLibraryCode/AmpBox_resources.rc"
    TARGET_DIRECTORY AmpBox_rc_lib
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/AmpBox_artefacts/JuceLibraryCode/icon.ico")

# JUCE arma icon.ico al configurar: si cambian los PNG, volver a configurar.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-small.png")
