#include "Converter.h"
#include "DspresetParser.h"

#include <model/Manifest.h>
#include <model/ManifestWriter.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace dmconv
{

namespace
{
// Output FLAC file name for an asset id ("flac:Bass_1C" -> "Bass_1C.flac").
juce::String flacFileNameForId (const juce::String& id)
{
    return id.fromLastOccurrenceOf (":", false, false) + ".flac";
}

// Losslessly transcode a WAV to FLAC. Never trims, pads, or resamples: writes
// exactly the frames the reader reports. `outFrames` returns that count and
// `formatNote` is set when the source bit-depth had to be represented as 24-bit
// (e.g. float sources) — purely informational, the audio is not altered.
bool transcodeToFlac (const juce::File& wav, const juce::File& outFlac,
                      juce::int64& outFrames, juce::String& formatNote, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wav));
    if (reader == nullptr)
    {
        error = "cannot read " + wav.getFullPathName();
        return false;
    }

    outFrames = reader->lengthInSamples;

    outFlac.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (outFlac.createOutputStream());
    if (out == nullptr || ! out->openedOk())
    {
        error = "cannot write " + outFlac.getFullPathName();
        return false;
    }

    const int sourceBits = (int) reader->bitsPerSample;
    int bits = sourceBits;
    if (bits != 16 && bits != 24)
    {
        bits = 24; // FLAC stores integers; float / odd depths → 24-bit
        formatNote = outFlac.getFileName() + ": source is "
                   + (reader->usesFloatingPointData ? juce::String ("float")
                                                     : juce::String (sourceBits) + "-bit")
                   + " — stored as 24-bit FLAC (audio unchanged within 24-bit range)";
    }

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        flac.createWriterFor (out.get(), reader->sampleRate,
                              reader->numChannels, bits, {}, 0));
    if (writer == nullptr)
    {
        error = "cannot create FLAC writer for " + outFlac.getFullPathName();
        return false;
    }
    out.release(); // writer owns the stream now

    const bool ok = writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples);
    writer.reset(); // flush + close
    if (! ok)
        error = "FLAC encode failed for " + outFlac.getFullPathName();
    return ok;
}

// Remove groups carrying a drop tag (e.g. VCCO's "A2" — a DecentSampler duplicate of "A"
// used only to double the level when double-track is off) and every binding targeting them.
// To preserve loudness, boost the `boostTag` groups ×2 in the double-track button's OFF state
// (they were being summed with the now-removed duplicate). Also auto-link the stereo button
// (all-PAN) to switch the double-track button on, so stereo is never a no-op mono. Per-mode +
// pattern-detected, so modes without the structure (e.g. the Lite preset) are left untouched.
void applyGroupDrops (dm::PresetLibrary& library, const ConvertOptions& options)
{
    if (options.dropGroupTags.isEmpty())
        return;

    auto hasDropTag = [&] (const dm::Group& g)
    {
        for (const auto& t : g.tags)
            if (options.dropGroupTags.contains (t))
                return true;
        return false;
    };
    auto isTrue = [] (const juce::var& v)
    {
        if (v.isBool()) return (bool) v;
        const auto s = v.toString();
        return s.equalsIgnoreCase ("true") || s.getFloatValue() > 0.5f;
    };

    for (auto& mode : library.modes)
    {
        juce::StringArray droppedUids;
        for (const auto& g : mode.groups)
            if (hasDropTag (g))
                droppedUids.add (g.uid);
        if (droppedUids.isEmpty() || mode.ui.tabs.isEmpty())
            continue;   // this mode doesn't have the structure (e.g. Lite) → leave it alone

        auto& tab = mode.ui.tabs.getReference (0);

        // Detect the double-track button (toggles ENABLED on a dropped group) + its OFF
        // state (the one that enables a dropped group), and the stereo button (all-PAN) + its
        // ON state (a non-zero PAN).
        int dtBtn = -1, dtOffState = -1, dtOnState = -1, stereoBtn = -1, stereoOnState = -1;
        for (int bi = 0; bi < tab.buttons.size(); ++bi)
        {
            const auto& btn = tab.buttons.getReference (bi);
            bool togglesDropped = false, allPan = ! btn.states.isEmpty();
            for (const auto& st : btn.states)
                for (const auto& b : st.bindings)
                {
                    if (b.parameter != "PAN") allPan = false;
                    if (b.parameter == "ENABLED" && droppedUids.contains (b.targetId)) togglesDropped = true;
                }

            if (togglesDropped && dtBtn < 0)
            {
                dtBtn = bi;
                for (int si = 0; si < btn.states.size(); ++si)
                    for (const auto& b : btn.states.getReference (si).bindings)
                        if (b.parameter == "ENABLED" && droppedUids.contains (b.targetId) && isTrue (b.translationValue))
                            dtOffState = si;
                for (int si = 0; si < btn.states.size(); ++si)
                    if (si != dtOffState) dtOnState = si;
            }
            if (allPan && stereoBtn < 0)
            {
                stereoBtn = bi;
                for (int si = 0; si < btn.states.size(); ++si)
                    for (const auto& b : btn.states.getReference (si).bindings)
                        if (b.parameter == "PAN" && b.translationValue.toString().getIntValue() != 0)
                            stereoOnState = si;
            }
        }
        int stereoOffState = -1;
        if (stereoBtn >= 0)
            for (int si = 0; si < tab.buttons.getReference (stereoBtn).states.size(); ++si)
                if (si != stereoOnState) stereoOffState = si;

        // Remove bindings targeting dropped groups (buttons + menus), then drop the groups.
        auto stripBindings = [&] (juce::Array<dm::Binding>& binds)
        {
            for (int i = binds.size() - 1; i >= 0; --i)
                if (droppedUids.contains (binds.getReference (i).targetId))
                    binds.remove (i);
        };
        for (auto& btn : tab.buttons)
            for (auto& st : btn.states)
                stripBindings (st.bindings);
        for (auto& menu : tab.menus)
            for (auto& opt : menu.options)
                stripBindings (opt.bindings);
        for (int i = mode.groups.size() - 1; i >= 0; --i)
            if (hasDropTag (mode.groups.getReference (i)))
                mode.groups.remove (i);

        // Loudness compensation: boost the always-on base groups ×2 in the DT OFF state
        // (they lose the coherent duplicate), ×1 in the ON state.
        if (dtBtn >= 0 && dtOffState >= 0 && dtOnState >= 0 && options.doubleTrackBoostTag.isNotEmpty())
        {
            auto& btn = tab.buttons.getReference (dtBtn);
            auto addVol = [&] (int stateIdx, const char* value)
            {
                for (const auto& g : mode.groups)
                    if (g.tags.contains (options.doubleTrackBoostTag))
                    {
                        dm::Binding b;
                        b.type = "amp"; b.level = "group"; b.targetId = g.uid;
                        b.parameter = "AMP_VOLUME"; b.translation = "fixed_value";
                        b.translationValue = juce::var (juce::String (value));
                        btn.states.getReference (stateIdx).bindings.add (b);
                    }
            };
            addVol (dtOffState, "2");
            addVol (dtOnState,  "1");
        }

        // Stereo level compensation: hard-panning leaves one track per channel (~6 dB
        // quieter than both centred). Boost the instrument level in the stereo ON state
        // (setMasterVolume — independent of the per-group DT comp), reset to 1 in OFF.
        if (stereoBtn >= 0 && stereoOnState >= 0 && stereoOffState >= 0)
        {
            auto& btn = tab.buttons.getReference (stereoBtn);
            auto addMaster = [&] (int stateIdx, const juce::String& value)
            {
                dm::Binding b;
                b.type = "amp"; b.level = "instrument";
                b.parameter = "AMP_VOLUME"; b.translation = "fixed_value";
                b.translationValue = juce::var (value);
                btn.states.getReference (stateIdx).bindings.add (b);
            };
            addMaster (stereoOnState,  juce::String (options.doubleTrackStereoBoost));
            addMaster (stereoOffState, "1");
        }

        // Keep the invariant "stereo ⟹ double-track": turning stereo on enables double-
        // track, and turning double-track off disables stereo (so stereo never pans a
        // disabled track to one side).
        if (stereoBtn >= 0 && dtBtn >= 0)
        {
            if (stereoOnState >= 0 && dtOnState >= 0)
                mode.ui.buttonLinks.add ({ stereoBtn, stereoOnState, dtBtn, dtOnState });
            if (dtOffState >= 0 && stereoOffState >= 0)
                mode.ui.buttonLinks.add ({ dtBtn, dtOffState, stereoBtn, stereoOffState });
        }
    }
}
} // namespace

ConvertResult convertLibrary (const ConvertOptions& options)
{
    ConvertResult result;

    if (! options.libraryDir.isDirectory())
    {
        result.errors.add ("library dir not found: " + options.libraryDir.getFullPathName());
        return result;
    }

    // 1. Choose preset files (filtered, name-sorted for deterministic mode order).
    juce::Array<juce::File> presetFiles;
    for (auto& f : options.libraryDir.findChildFiles (juce::File::findFiles, false, "*.dspreset"))
        if (options.presetFilter.isEmpty()
            || options.presetFilter.contains (f.getFileNameWithoutExtension(), true))
            presetFiles.add (f);

    struct ByName
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareNatural (b.getFileName());
        }
    };
    ByName cmp;
    presetFiles.sort (cmp);

    if (presetFiles.isEmpty())
    {
        result.errors.add ("no matching .dspreset files in " + options.libraryDir.getFullPathName());
        return result;
    }

    // 2. Parse presets → modes, collecting all referenced assets.
    dm::PresetLibrary library;
    library.schema  = dm::kManifestSchemaVersion;
    library.format  = "dmse-manifest";
    library.library = options.libraryName.isNotEmpty()
                          ? options.libraryName
                          : options.libraryDir.getFileName();
    if (options.gainDb.has_value())
        library.gainDb = *options.gainDb;

    juce::StringPairArray assets; // id -> library-relative path (deduped across modes)

    for (const auto& file : presetFiles)
    {
        const auto name = file.getFileNameWithoutExtension();
        auto parsed = dmconv::parseDspreset (file.loadFileAsString(), name);
        if (! parsed.ok)
        {
            for (auto& e : parsed.errors)
                result.errors.add (name + ": " + e);
            continue;
        }
        for (auto& w : parsed.warnings)
            result.warnings.add (name + ": " + w);
        library.modes.add (parsed.mode);
        for (auto& key : parsed.assets.getAllKeys())
            assets.set (key, parsed.assets[key]);
        result.log.add ("parsed " + name + " (" + juce::String (parsed.mode.groups.size()) + " groups)");
    }

    if (library.modes.isEmpty())
    {
        result.errors.add ("no presets parsed successfully");
        return result;
    }

    // The audio file is authoritative for length: we record each asset's actual
    // decoded frame count and write it into the manifest below (the .dspreset
    // `length` values are unreliable). The audio itself is never trimmed or padded.
    juce::HashMap<juce::String, juce::int64> actualFrames;

    // 3. Transcode assets → FLAC, sorted into subdirectories by kind so the repo
    //    can .gitignore just the audio (samples/ + ir/) and commit images + manifest.
    if (auto dirResult = options.outDir.createDirectory(); dirResult.failed())
    {
        result.errors.add ("cannot create out dir: " + dirResult.getErrorMessage());
        return result;
    }

    const auto samplesDir = options.outDir.getChildFile ("samples");
    const auto irDir      = options.outDir.getChildFile ("ir");
    const auto imagesDir  = options.outDir.getChildFile ("images");

    // Clean stale generated assets (re-runs, and the old flat layout where audio +
    // images sat directly in assets/). The manifest at the root is overwritten below.
    for (auto& f : options.outDir.findChildFiles (juce::File::findFiles, false,
                                                  "*.flac;*.png;*.jpg;*.jpeg"))
        f.deleteFile();
    samplesDir.deleteRecursively();
    irDir.deleteRecursively();
    imagesDir.deleteRecursively();
    samplesDir.createDirectory();
    irDir.createDirectory();
    imagesDir.createDirectory();

    // Sample pack (--pack-samples): concatenate every sample FLAC into one file the
    // plugin memory-maps, plus a JSON index of [id, offset, length]. Avoids compiling a
    // multi-GB library into the binary. IRs + images stay individual (small → embedded).
    const bool packing = options.packSamples;
    const auto packFile      = samplesDir.getChildFile ("samples.pak");
    const auto packIndexFile = samplesDir.getChildFile ("samples.pak.json");
    std::unique_ptr<juce::FileOutputStream> packOut;
    juce::Array<juce::var> packEntries;
    if (packing)
    {
        packFile.deleteFile();
        packOut = packFile.createOutputStream();
        if (packOut == nullptr || ! packOut->openedOk())
            result.errors.add ("cannot open sample pack for writing: " + packFile.getFullPathName());
    }

    for (auto& id : assets.getAllKeys())
    {
        const auto src = options.libraryDir.getChildFile (assets[id]);

        if (! src.existsAsFile())
        {
            result.errors.add ("missing source asset: " + src.getFullPathName());
            continue;
        }

        // Images (UI assets) are embedded verbatim, keeping their filename; audio
        // (samples and IRs) is transcoded to FLAC. Each kind goes in its own subdir.
        if (id.startsWith ("img:"))
        {
            const auto dst = imagesDir.getChildFile (src.getFileName());
            dst.deleteFile();
            if (src.copyFileTo (dst))
            {
                ++result.assetsTranscoded;
                result.log.add ("img   images/" + dst.getFileName());
            }
            else
            {
                result.errors.add ("cannot copy " + src.getFullPathName());
            }
            continue;
        }

        const bool isIr = id.startsWith ("ir:");

        // Packed samples: transcode to a temp FLAC (proven path), append its bytes to the
        // pack, and record the slice. IRs are never packed (they stay embedded).
        if (packing && ! isIr && packOut != nullptr)
        {
            const auto tmp = samplesDir.getChildFile ("~pack_tmp.flac");
            juce::int64 frames = 0;
            juce::String formatNote, error;
            if (transcodeToFlac (src, tmp, frames, formatNote, error))
            {
                juce::MemoryBlock mb;
                if (tmp.loadFileAsData (mb))
                {
                    const juce::int64 offset = packOut->getPosition();
                    packOut->write (mb.getData(), mb.getSize());
                    auto* e = new juce::DynamicObject();
                    e->setProperty ("id", id);
                    e->setProperty ("o", offset);
                    e->setProperty ("l", (juce::int64) mb.getSize());
                    packEntries.add (juce::var (e));
                    ++result.assetsTranscoded;
                    actualFrames.set (id, frames);
                }
                else
                    result.errors.add ("cannot read temp FLAC for packing: " + id);
                if (formatNote.isNotEmpty())
                    result.warnings.add (formatNote);
                tmp.deleteFile();
            }
            else
                result.errors.add (error);
            continue;
        }

        const auto dst = (isIr ? irDir : samplesDir).getChildFile (flacFileNameForId (id));
        juce::int64 frames = 0;
        juce::String formatNote, error;
        if (transcodeToFlac (src, dst, frames, formatNote, error))
        {
            ++result.assetsTranscoded;
            result.log.add ((isIr ? "ir    ir/" : "flac  samples/") + dst.getFileName());

            if (formatNote.isNotEmpty())
                result.warnings.add (formatNote);

            actualFrames.set (id, frames);
        }
        else
        {
            result.errors.add (error);
        }
    }

    // Finalise the pack: flush the data and write the index next to it.
    if (packing && packOut != nullptr)
    {
        packOut->flush();
        packOut.reset();
        packIndexFile.replaceWithText (juce::JSON::toString (juce::var (packEntries)));
        result.log.add ("pack  samples/samples.pak (" + juce::String (packEntries.size())
                        + " samples) + samples.pak.json");
    }

    // Make the decoded audio length authoritative in the manifest. We note — never
    // alter — any sample whose .dspreset `length` disagreed.
    int lengthOverrides = 0;
    juce::String overrideExample;
    for (int mi = 0; mi < library.modes.size(); ++mi)
    {
        auto& m = library.modes.getReference (mi);
        for (int gi = 0; gi < m.groups.size(); ++gi)
        {
            auto& g = m.groups.getReference (gi);
            for (int si = 0; si < g.samples.size(); ++si)
            {
                auto& s = g.samples.getReference (si);
                if (! actualFrames.contains (s.source))
                    continue;

                const auto actual = (int) actualFrames[s.source];
                if (s.lengthFrames.has_value() && *s.lengthFrames != actual)
                {
                    ++lengthOverrides;
                    if (overrideExample.isEmpty())
                        overrideExample = flacFileNameForId (s.source) + ": " + juce::String (actual)
                                        + " vs declared " + juce::String (*s.lengthFrames);
                }
                s.lengthFrames = actual;
            }
        }
    }
    if (lengthOverrides > 0)
        result.warnings.add ("used actual audio length for " + juce::String (lengthOverrides)
            + " sample(s) where the .dspreset declared a different value (e.g. "
            + overrideExample + "); audio unchanged");

    // Disable loops whose points fall outside the actual audio (e.g. authored
    // against a stale .dspreset length). Manifest honesty — the audio is untouched;
    // the engine also clamps defensively at load.
    int loopIssues = 0;
    juce::String loopExample;
    for (int mi = 0; mi < library.modes.size(); ++mi)
    {
        auto& m = library.modes.getReference (mi);
        for (int gi = 0; gi < m.groups.size(); ++gi)
        {
            auto& g = m.groups.getReference (gi);
            for (int si = 0; si < g.samples.size(); ++si)
            {
                auto& s = g.samples.getReference (si);
                if (! s.loop.enabled)
                    continue;

                const juce::int64 frames = actualFrames.contains (s.source)
                                               ? actualFrames[s.source]
                                               : (juce::int64) s.lengthFrames.value_or (0);
                const int st = s.loop.start.value_or (0);
                const int en = s.loop.end.value_or (0);

                if (frames > 0 && (st < 0 || en <= st || (juce::int64) en > frames))
                {
                    s.loop.enabled = false;
                    ++loopIssues;
                    if (loopExample.isEmpty())
                        loopExample = flacFileNameForId (s.source) + ": loop " + juce::String (st)
                                    + ".." + juce::String (en) + " vs " + juce::String (frames) + " frames";
                }
            }
        }
    }
    if (loopIssues > 0)
        result.warnings.add ("disabled " + juce::String (loopIssues)
            + " out-of-range loop(s) (e.g. " + loopExample
            + "); re-author the loop points in the source — audio unchanged");

    // Optional reverb wet trim: bake the requested dB into every convolution
    // effect's outputLevel so the engine balances the normalised IR by default.
    if (options.reverbGainDb.has_value())
    {
        int n = 0;
        for (int mi = 0; mi < library.modes.size(); ++mi)
        {
            auto& m = library.modes.getReference (mi);
            for (int ei = 0; ei < m.effects.size(); ++ei)
            {
                auto& e = m.effects.getReference (ei);
                if (e.type == "convolution")
                {
                    e.outputLevel = *options.reverbGainDb;
                    ++n;
                }
            }
        }
        if (n > 0)
            result.log.add ("reverb wet gain " + juce::String (*options.reverbGainDb, 1)
                            + " dB applied to " + juce::String (n) + " convolution effect(s)");
    }

    if (options.gainDb.has_value())
        result.log.add ("library pre-FX gain trim: " + juce::String (*options.gainDb, 1) + " dB");

    if (! options.normalizeIr)
    {
        int n = 0;
        for (int mi = 0; mi < library.modes.size(); ++mi)
        {
            auto& m = library.modes.getReference (mi);
            for (int ei = 0; ei < m.effects.size(); ++ei)
            {
                auto& e = m.effects.getReference (ei);
                if (e.type == "convolution") { e.normalizeIr = false; ++n; }
            }
        }
        if (n > 0)
            result.log.add ("IR normalisation OFF for " + juce::String (n) + " convolution effect(s)");
    }

    // Shift UI element y so manifest coordinates are background-relative (the
    // DecentSampler menu bar offset — see ConvertOptions::uiYOffset).
    if (options.uiYOffset != 0)
        for (int mi = 0; mi < library.modes.size(); ++mi)
        {
            auto& ui = library.modes.getReference (mi).ui;
            for (int ti = 0; ti < ui.tabs.size(); ++ti)
            {
                auto& t = ui.tabs.getReference (ti);
                for (int i = 0; i < t.controls.size(); ++i) t.controls.getReference (i).rect.y += options.uiYOffset;
                for (int i = 0; i < t.buttons.size();  ++i) t.buttons.getReference (i).rect.y  += options.uiYOffset;
                for (int i = 0; i < t.images.size();   ++i) t.images.getReference (i).rect.y   += options.uiYOffset;
                for (int i = 0; i < t.menus.size();    ++i) t.menus.getReference (i).rect.y    += options.uiYOffset;
            }
        }

    // Per-mode top crop (engine applies it: trims the background top, shrinks height,
    // shifts elements up). Keyed by mode name; cropTopDefault covers the rest.
    for (int mi = 0; mi < library.modes.size(); ++mi)
    {
        auto& mode = library.modes.getReference (mi);
        const auto it = options.cropTopByMode.find (mode.name);
        mode.ui.cropTop = (it != options.cropTopByMode.end()) ? it->second : options.cropTopDefault;

        // Keyboard-colour override (config only): replace the parsed colours for this
        // mode. Per-mode entry wins; else the default list (if any) applies.
        const auto kit = options.keyboardColorsByMode.find (mode.name);
        if (kit != options.keyboardColorsByMode.end() || options.haveKeyboardDefault)
        {
            const auto& colors = (kit != options.keyboardColorsByMode.end()) ? kit->second
                                                                             : options.keyboardColorsDefault;
            mode.ui.keyboardColors.clearQuick();
            for (const auto& kc : colors)
                mode.ui.keyboardColors.add (kc);
        }

        // Global per-key-type tint (config only): applies to every mode.
        mode.ui.whiteKeyTint = options.whiteKeyTint;
        mode.ui.blackKeyTint = options.blackKeyTint;
    }

    // Drop DecentSampler duplicate groups (e.g. A2), compensate loudness, and auto-link
    // stereo → double-track. No-op unless dropGroupTags is set.
    applyGroupDrops (library, options);

    // 4. Write the manifest as a SPLIT folder (index.json + modes/<name>.json +
    //    optional partials/) — this is what the plugins embed and load, and it's
    //    readable / diffable / hand-editable. No single manifest.json is written;
    //    the engine's single-file loader still exists (tests / API), but the
    //    converter's output is split-only.
    const auto manifestDir = options.outDir.getChildFile ("manifest");
    result.manifestFile = manifestDir.getChildFile ("index.json");
    if (! dm::writeSplitManifest (library, manifestDir))
        result.errors.add ("cannot write manifest folder: " + manifestDir.getFullPathName());

    // Drop any stale single manifest.json left by an older converter run.
    options.outDir.getChildFile ("manifest.json").deleteFile();

    result.modes = library.modes.size();
    result.ok = result.errors.isEmpty();
    return result;
}

} // namespace dmconv
