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
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.size() < 2)
    {
        std::cout << "usage: dmse-convert <library-dir> <out-dir> [preset ...]\n"
                  << "  Converts DecentSampler .dspreset presets into the engine\n"
                  << "  JSON manifest + FLAC bundle in <out-dir>.\n";
        return 2;
    }

    dmconv::ConvertOptions opts;
    opts.libraryDir = resolvePath (args[0]);
    opts.outDir     = resolvePath (args[1]);
    for (int i = 2; i < args.size(); ++i)
        opts.presetFilter.add (args[i]);

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
