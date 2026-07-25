#pragma once

// ds-plugin-converter — orchestration.
//
// Parses the selected `.dspreset` presets of a DecentSampler library into a
// single engine manifest, transcodes every referenced WAV (and IR) to FLAC in the
// output dir, and writes the manifest JSON via the engine's ManifestWriter.

#include <juce_core/juce_core.h>
#include <model/Manifest.h>
#include <optional>
#include <map>
#include <vector>

namespace dmconv
{

struct ConvertOptions
{
    // Force a full re-transcode of every sample, bypassing the incremental cache
    // (CLI --force). Normally samples are re-encoded only when a source WAV changes.
    bool forceRetranscode = false;

    juce::File libraryDir;            // source library root (holds *.dspreset + Samples/ + ...)
    juce::File outDir;                // where FLAC bundle + manifest.json are written
    juce::String libraryName;         // manifest "library" name (default: libraryDir name)
    juce::StringArray presetFilter;   // mode stems to include; empty = all *.dspreset

    // When set, written as `effect.outputLevel` (dB) on every convolution effect,
    // so the engine trims the normalised reverb wet to taste. Library-specific
    // (the IR normalisation offset differs per library), hence opt-in.
    std::optional<double> reverbGainDb;

    // Library-wide level trim (dB) written to the manifest's top-level `gainDb`; the
    // engine applies it to the voice mix BEFORE the FX, so the drive/amp sees the
    // same signal level DecentSampler feeds it (our voice sum runs hotter than DS).
    std::optional<double> gainDb;

    // Default for the Poly-save toggle (skip silent groups at note-on). Set false for
    // libraries whose controls blend muted groups in mid-note (config "polySaveDefault").
    bool polySaveDefault = true;

    // Default for the Retrigger-mute toggle (one voice per key within a group). Set false
    // for libraries where stacking the same key is wanted (config "retriggerMuteDefault").
    bool retriggerMuteDefault = true;

    // Omnichord-style select+strum (config "omnichordStrum"): rewrite each mode's
    // chord-order key-switches into `strumKeys` carrying their menu option's
    // sequence-index offset. Chord keys then only SELECT the chord; the strum keys
    // fire it in their note order, and a chord change morphs still-ringing notes.
    // Also removes the (now inert) chord-order menu from the UI and emits
    // keyboard labels: chord-type section names (derived from the sequence names)
    // and per-strum-key captions.
    bool omnichordStrum = false;

    // Shared-air ("fan") simulation (config "airSupply": {volume,brightness,attack},
    // all optional) — written to the manifest's library-level airSupply, which makes
    // the engine offer the settings toggle. For Elektrisk Salmesykkel (Yamaha L-20D).
    std::optional<dm::AirSupply> airSupply;

    // Per-mode background override (config "backgroundFromMode": { "Mode": "OtherMode" }):
    // the mode borrows OTHER MODE's background image. Plugin-only styling — e.g.
    // Omni-84's AutoStrum keeps its keyswitch-legend background in DecentSampler,
    // but the plugin renders live keyboard labels and uses the plain background.
    std::map<juce::String, juce::String> backgroundFromMode;

    // Per-mode overlay image (config "overlay"): a mostly-transparent PNG (library-
    // relative path, e.g. "Resources/glass.png") drawn OVER the background and every
    // control, without blocking them. Plugin-only styling. A string applies to every
    // mode (overlayDefault); an object { "default": "...", "ModeName": "..." } keys it
    // per mode. Empty = no overlay.
    juce::String overlayDefault;
    bool haveOverlayDefault = false;
    std::map<juce::String, juce::String> overlayByMode;

    // How far an overlay reaches (config "overlayScope"): "" / "face" (the face only) or
    // "instrument" (the face plus the keyboard, excluding the top and bottom bars).
    // Applied to every mode that gets an overlay.
    juce::String overlayScope;

    // Captions for the strum keys, in key-switch order (config "strumKeyLabels",
    // e.g. ["↑","↓","↑*","↓*"]). Empty / missing entries fall back to the menu
    // option's name — usually too wide for a single key, hence this override.
    juce::StringArray strumKeyLabels;

    // When false, convolution IRs are used as recorded (Normalise::no) instead of
    // energy-normalised — matches DecentSampler, which doesn't normalise. Needed for
    // cabinet IRs (normalising attenuates them ~14 dB). Per-library, hence opt-out.
    bool normalizeIr = true;

    // When true (default), sample FLACs are concatenated into a single pack
    // (samples/samples.pak + samples.pak.json index) instead of individual files, so the
    // plugin memory-maps them from disk rather than compiling them into the binary — small
    // binaries, fast launch, low RAM, no per-format duplication. IRs + images stay
    // individual (small → embedded). Opt out with --no-pack-samples / "packSamples": false.
    bool packSamples = true;

    // Added to every UI element's y so manifest coords are background-relative:
    // DecentSampler positions controls below its menu bar that the bg image spans
    // but our renderer doesn't draw. ~50 in the half-scaled UI logical space
    // (the menu bar is ~100 px in the 2× background image).
    int uiYOffset = 50;

    // Per-mode top crop (design px) written to each mode's `ui.cropTop`: the engine
    // trims that much off the top of the background, shrinks the UI, and shifts every
    // element up (reclaims the dead menu-bar header where it isn't wanted). Keyed by
    // mode name (= preset filename stem); `cropTopDefault` applies to any mode not
    // listed. From `--crop-top` (e.g. "60" or "60,Split=0").
    int cropTopDefault = 0;
    std::map<juce::String, int> cropTopByMode;

    // Keyboard-colour override (config only — DecentSampler ignores the config file, so
    // its preset's `<keyboard>` is untouched). When present for a mode, REPLACES that
    // mode's parsed keyboard colours. `keyboardColorsDefault` (with haveKeyboardDefault)
    // applies to any mode not in `keyboardColorsByMode`. From dmse-convert.json's
    // "keyboardColors" (a flat array = all modes, or { "default"/"ModeName": [...] }).
    bool haveKeyboardDefault = false;
    std::vector<dm::KeyboardColor> keyboardColorsDefault;
    std::map<juce::String, std::vector<dm::KeyboardColor>> keyboardColorsByMode;

    // Keyboard captions strip (config "keyboardLabels", same shape as
    // "keyboardColors": flat array = default for every mode, or a
    // { "default": [...], "ModeName": [...] } map). A per-mode entry REPLACES that
    // mode's labels — including ones auto-derived by omnichordStrum.
    bool haveKeyboardLabelsDefault = false;
    std::vector<dm::KeyboardLabel> keyboardLabelsDefault;
    std::map<juce::String, std::vector<dm::KeyboardLabel>> keyboardLabelsByMode;

    // Global per-key-type keyboard tint (ARGB hex; alpha = strength), applied to every
    // mode. Overlaid on ALL white / black keys — e.g. whiteKeyTint "30ffcc00" = subtle
    // yellow on white keys only. From dmse-convert.json "whiteKeyTint"/"blackKeyTint".
    juce::String whiteKeyTint;
    juce::String blackKeyTint;

    // Per-plugin dropdown-popup colours (config "menuPopupColors": { background, text,
    // highlight, highlightText }; ARGB hex). Style the list that drops down from in-GUI
    // select menus. Applied to every mode. Empty = default look.
    juce::String menuPopupBackground;
    juce::String menuPopupText;
    juce::String menuPopupHighlight;
    juce::String menuPopupHighlightText;

    // Drop groups carrying any of these tags + all bindings targeting them. Used to remove
    // DecentSampler workaround duplicates (e.g. VCCO's "A2" = a copy of "A" for the double-
    // track-off state). From dmse-convert.json "dropGroupTags".
    juce::StringArray dropGroupTags;

    // When dropGroupTags removes a coherent duplicate (A2), the double-track-off state loses
    // its level-doubling. Boost groups with THIS tag ×2 in the double-track button's OFF
    // state (and ×1 in ON) to preserve the loudness. Also auto-links the stereo button (all-
    // PAN) to turn the double-track button on. From "doubleTrackBoostTag".
    juce::String doubleTrackBoostTag;

    // Stereo level compensation: hard-panning the two tracks L/R leaves one track per channel
    // (~6 dB quieter than both centred). The stereo button's ON state multiplies the instrument
    // level by this to compensate; ON=off leaves it at 1. Tune by ear. From
    // "doubleTrackStereoBoost" (default 2.0 = +6 dB).
    double doubleTrackStereoBoost = 2.0;
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
