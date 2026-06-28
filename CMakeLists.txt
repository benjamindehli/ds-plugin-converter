# ds-plugin-converter — build-time CLI: DecentSampler .dspreset → engine manifest
# + FLAC asset bundle. Not shipped; links the engine for its model + ManifestWriter.

juce_add_console_app(dmse_convert PRODUCT_NAME "dmse-convert")

target_sources(dmse_convert PRIVATE
    source/DspresetParser.cpp
    source/Converter.cpp
    source/Main.cpp
)

target_link_libraries(dmse_convert PRIVATE
    dehli_musikk_sampler_engine
    juce::juce_core
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(dmse_convert PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)

# ---------------------------------------------------------------------------
# Parser unit tests — parse the fixture presets (no WAVs needed) and round-trip
# through the engine writer/loader.
# ---------------------------------------------------------------------------
option(DMSE_CONVERTER_BUILD_TESTS "Build ds-plugin-converter unit tests" ON)

if(DMSE_CONVERTER_BUILD_TESTS AND COMMAND juce_add_console_app)
    juce_add_console_app(dmse_convert_tests PRODUCT_NAME "dmse-convert-tests")

    target_sources(dmse_convert_tests PRIVATE
        source/DspresetParser.cpp
        source/Converter.cpp
        tests/DspresetParserTests.cpp
        tests/ConverterTests.cpp
    )

    target_link_libraries(dmse_convert_tests PRIVATE
        dehli_musikk_sampler_engine
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    target_compile_definitions(dmse_convert_tests PRIVATE
        DMSE_CONVERTER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    enable_testing()
    add_test(NAME dmse_convert_tests COMMAND dmse_convert_tests)
endif()
