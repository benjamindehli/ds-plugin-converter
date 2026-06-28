#pragma once

// ds-plugin-converter — orchestration.
//
// Parses the selected `.dspreset` presets of a DecentSampler library into a
// single engine manifest, transcodes every referenced WAV (and IR) to FLAC in the
// output dir, and writes the manifest JSON via the engine's ManifestWriter.

#include <juce_core/juce_core.h>
#include <optional>

namespace dmconv
{

struct ConvertOptions
{
    juce::File libraryDir;            // source library root (holds *.dspreset + Samples/ + ...)
    juce::File outDir;                // where FLAC bundle + manifest.json are written
    juce::String libraryName;         // manifest "library" name (default: libraryDir name)
    juce::StringArray presetFilter;   // mode stems to include; empty = all *.dspreset

    // When set, written as `effect.outputLevel` (dB) on every convolution effect,
    // so the engine trims the normalised reverb wet to taste. Library-specific
    // (the IR normalisation offset differs per library), hence opt-in.
    std::optional<double> reverbGainDb;

    // Added to every UI element's y so manifest coords are background-relative:
    // DecentSampler positions controls below its menu bar that the bg image spans
    // but our renderer doesn't draw. ~50 in the half-scaled UI logical space
    // (the menu bar is ~100 px in the 2× background image).
    int uiYOffset = 50;
};

struct ConvertResult
{
    bool ok = false;
    juce::File manifestFile;
    int modes = 0;
    int assetsTranscoded = 0;
    juce::StringArray log;
    juce::StringArray warnings;   // non-fatal: e.g. WAV length ≠ .dspreset length
    juce::StringArray errors;
};

ConvertResult convertLibrary (const ConvertOptions& options);

} // namespace dmconv
