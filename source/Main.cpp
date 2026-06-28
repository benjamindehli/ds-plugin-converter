// ds-plugin-converter — CLI entry point.
//
//   dmse-convert <library-dir> <out-dir> [preset ...]
//
// Converts a DecentSampler library into the engine's JSON manifest + a FLAC asset
// bundle in <out-dir>. With no preset names, every *.dspreset in the library is
// converted; otherwise only the named ones (by file-name stem, e.g. "Bass").

#include "Converter.h"
#include <juce_core/juce_core.h>
#include <iostream>
#include <optional>

namespace
{
juce::File resolvePath (const juce::String& arg)
{
    return juce::File::isAbsolutePath (arg)
               ? juce::File (arg)
               : juce::File::getCurrentWorkingDirectory().getChildFile (arg);
}
}

int main (int argc, char* argv[])
{
    juce::StringArray positional;
    std::optional<double> reverbGainDb;
    std::optional<int> uiYOffset;

    for (int i = 1; i < argc; ++i)
    {
        const auto arg = juce::String::fromUTF8 (argv[i]);
        if (arg == "--reverb-gain" && i + 1 < argc)
            reverbGainDb = juce::String::fromUTF8 (argv[++i]).getDoubleValue();
        else if (arg == "--ui-y-offset" && i + 1 < argc)
            uiYOffset = juce::String::fromUTF8 (argv[++i]).getIntValue();
        else
            positional.add (arg);
    }

    if (positional.size() < 2)
    {
        std::cout << "usage: dmse-convert <library-dir> <out-dir> [preset ...] [--reverb-gain <dB>]\n"
                  << "  Converts DecentSampler .dspreset presets into the engine\n"
                  << "  JSON manifest + FLAC bundle in <out-dir>.\n"
                  << "  --reverb-gain <dB>   trim every convolution wet (e.g. 12 for Omni-84).\n"
                  << "  --ui-y-offset <px>   shift UI elements down (default 100; menu-bar offset).\n";
        return 2;
    }

    dmconv::ConvertOptions opts;
    opts.libraryDir   = resolvePath (positional[0]);
    opts.outDir       = resolvePath (positional[1]);
    opts.reverbGainDb = reverbGainDb;
    if (uiYOffset.has_value())
        opts.uiYOffset = *uiYOffset;
    for (int i = 2; i < positional.size(); ++i)
        opts.presetFilter.add (positional[i]);

    const auto result = dmconv::convertLibrary (opts);

    for (auto& line : result.log)
        std::cout << "  " << line << "\n";

    if (! result.warnings.isEmpty())
    {
        std::cout << "\nwarnings (" << result.warnings.size() << "):\n";
        for (auto& w : result.warnings)
            std::cout << "  ~ " << w << "\n";
    }

    if (! result.errors.isEmpty())
    {
        std::cout << "\nerrors:\n";
        for (auto& e : result.errors)
            std::cout << "  ! " << e << "\n";
    }

    if (result.ok)
        std::cout << "\nOK — " << result.modes << " mode(s), "
                  << result.assetsTranscoded << " asset(s) → "
                  << result.manifestFile.getFullPathName() << "\n";
    else
        std::cout << "\nFAILED\n";

    return result.ok ? 0 : 1;
}
