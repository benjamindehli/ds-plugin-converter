#pragma once

// ds-plugin-converter — DecentSampler `.dspreset` XML parser.
//
// This is the ONLY component that understands `.dspreset`. It maps one preset
// onto the engine's data model (dm::Mode) and records the asset ids it
// references (samples + IRs) so the converter can transcode + embed them. The
// engine never sees this format.
//
// Pure on text (no file I/O) so it is unit-testable against the fixture
// presets.

#include <juce_core/juce_core.h>
#include <model/Manifest.h>

namespace dmconv {

struct ParseResult {
  bool ok = false;
  dm::Mode mode;

  // assetId -> library-relative source path, e.g.
  //   "flac:Bass_1C" -> "Samples/Bass/Bass_1C.wav"
  //   "ir:Space"     -> "IR/Space.wav"
  juce::StringPairArray assets;

  juce::StringArray errors;
  juce::StringArray
      warnings; // non-fatal (e.g. a CC binding targeting an unknown control)
};

/** Parse a `.dspreset` document. `modeName` is the mode label (normally the
    preset's file-name stem, since `.dspreset` has no name field). */
ParseResult parseDspreset(const juce::String &xmlText,
                          const juce::String &modeName);

} // namespace dmconv
