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

// Frame count of an existing FLAC (STREAMINFO only, no decode) — used by the
// incremental fast path to fill in sample lengths without re-transcoding.
juce::int64 flacFrameCount (const juce::File& flac)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (flac));
    return r != nullptr ? r->lengthInSamples : 0;
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
                mode.ui.buttonLinks.add ({ stereoBtn, stereoOnState, dtBtn, dtOnState, {}, {} });
            if (dtOffState >= 0 && stereoOffState >= 0)
                mode.ui.buttonLinks.add ({ dtBtn, dtOffState, stereoBtn, stereoOffState, {}, {} });
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
    library.polySaveDefault = options.polySaveDefault;
    library.retriggerMuteDefault = options.retriggerMuteDefault;
    library.airSupply = options.airSupply;   // shared-air simulation (settings toggle)

    juce::StringPairArray assets; // id -> library-relative path (deduped across modes)

    for (const auto& file : presetFiles)
    {
        const auto name = file.getFileNameWithoutExtension();
        auto parsed = dmconv::parseDspreset (file.loadFileAsString(), name);
        for (auto& w : parsed.warnings)   // forward warnings even for a failed parse
            result.warnings.add (name + ": " + w);
        if (! parsed.ok)
        {
            for (auto& e : parsed.errors)
                result.errors.add (name + ": " + e);
            result.errors.add (name + ": preset SKIPPED (not converted)");
            continue;
        }
        library.modes.add (parsed.mode);
        for (auto& key : parsed.assets.getAllKeys())
        {
            // Same basename-keyed id space across ALL modes: two presets referencing
            // different files with the same stem would make one of them play/show the
            // wrong asset. Modes legitimately share files (that's the dedup), so only
            // a DIFFERENT path for an existing id is an error.
            const auto existing = assets.getValue (key, {});
            if (existing.isNotEmpty() && existing != parsed.assets[key])
                result.errors.add (name + ": asset id collision across presets: '" + key
                                   + "' is \"" + existing + "\" elsewhere but \""
                                   + parsed.assets[key] + "\" here — rename one file");
            else
                assets.set (key, parsed.assets[key]);
        }
        result.log.add ("parsed " + name + " (" + juce::String (parsed.mode.groups.size()) + " groups)");
    }

    if (library.modes.isEmpty())
    {
        result.errors.add ("no presets parsed successfully");
        return result;
    }

    // Omnichord select+strum rewrite (config "omnichordStrum"): each chord-order
    // key-switch becomes a strum key carrying its menu option's sequence-index
    // offset, and clearing menuKeySwitches leaves the chord keys (sequenceTriggers)
    // as pure selectors — the engine switches behaviour on strumKeys' presence.
    // The chord-order dropdown is REMOVED from the UI (ordering now comes from
    // which strum key is played), and keyboard labels are emitted so the GUI shows
    // what the strum keys do and which chord type each key section selects.
    if (options.omnichordStrum)
        for (auto& mode : library.modes)
        {
            if (mode.menuKeySwitches.isEmpty() || mode.sequenceTriggers.isEmpty()
                || mode.ui.tabs.isEmpty() || mode.ui.tabs.getReference (0).menus.isEmpty())
                continue;
            // First menu of the first tab = the chord-order menu (same structural
            // rule the engine's key-switch → menu binding uses).
            auto& menus = mode.ui.tabs.getReference (0).menus;
            const auto menu = menus.getReference (0);   // copy — removed from the UI below
            int ksIndex = 0;
            for (const auto& ks : mode.menuKeySwitches)
            {
                if (ks.option < 0 || ks.option >= menu.options.size())
                {
                    result.warnings.add (mode.name + ": omnichordStrum: key-switch note "
                                         + juce::String (ks.note) + " selects missing menu option "
                                         + juce::String (ks.option + 1) + " — skipped");
                    ++ksIndex;
                    continue;
                }
                dm::StrumKey sk;
                sk.note      = ks.note;
                sk.seqOffset = menu.options.getReference (ks.option).seqIndex;
                mode.strumKeys.add (sk);

                // Caption over the strum key: recipe "strumKeyLabels" (short, fits a
                // single key) or the menu option's name as a fallback.
                dm::KeyboardLabel kl;
                kl.loNote = kl.hiNote = ks.note;
                kl.text = ksIndex < options.strumKeyLabels.size()
                              ? options.strumKeyLabels[ksIndex]
                              : menu.options.getReference (ks.option).name;
                mode.ui.keyboardLabels.add (kl);
                ++ksIndex;
            }
            mode.menuKeySwitches.clear();
            menus.remove (0);   // inert now — ordering comes from the strum keys
            // The background keeps the menu's black ribbon — repurpose that spot as
            // the live strum-speed readout (note value / steps/s) the editor renders.
            mode.ui.strumSpeedReadout = menu.rect;

            // Chord-type section labels: each trigger's sequence name minus its root
            // note prefix ("C#Minor7" → "Minor7"); consecutive chord keys sharing the
            // same remainder merge into one labelled range (e.g. 36–47 "Major").
            auto chordType = [] (const juce::String& seqName)
            {
                if (seqName.isNotEmpty() && seqName[0] >= 'A' && seqName[0] <= 'G')
                {
                    int n = 1;
                    if (seqName.length() > 1 && (seqName[1] == '#' || seqName[1] == 'b'))
                        ++n;
                    return seqName.substring (n);
                }
                return seqName;
            };
            dm::KeyboardLabel section;
            section.loNote = -1;
            for (const auto& t : mode.sequenceTriggers)
            {
                if (t.sequence < 0 || t.sequence >= mode.sequences.size())
                    continue;
                const auto type = chordType (mode.sequences.getReference (t.sequence).name);
                if (section.loNote >= 0 && type == section.text && t.note == section.hiNote + 1)
                {
                    section.hiNote = t.note;   // extend the current run
                    continue;
                }
                if (section.loNote >= 0)
                    mode.ui.keyboardLabels.add (section);
                section.loNote = section.hiNote = t.note;
                section.text = type;
            }
            if (section.loNote >= 0)
                mode.ui.keyboardLabels.add (section);

            result.log.add (mode.name + ": omnichord select+strum ("
                            + juce::String (mode.strumKeys.size()) + " strum keys, "
                            + juce::String (mode.ui.keyboardLabels.size())
                            + " keyboard labels, chord-order menu removed)");
        }

    // Background borrowed from another mode (config "backgroundFromMode") — plugin-only
    // styling; the .dspreset (and the DS edition) keep their own. Must run BEFORE the
    // asset transcode so a background nothing references any more is never emitted.
    for (auto& mode : library.modes)
        if (const auto bgIt = options.backgroundFromMode.find (mode.name);
            bgIt != options.backgroundFromMode.end())
        {
            bool found = false;
            for (const auto& other : library.modes)
                if (other.name == bgIt->second)
                {
                    mode.ui.background = other.ui.background;
                    found = true;
                    result.log.add (mode.name + ": background from mode \"" + bgIt->second + "\"");
                    break;
                }
            if (! found)
                result.warnings.add (mode.name + ": backgroundFromMode source mode \""
                                     + bgIt->second + "\" not found - background unchanged");
        }

    // Overrides above can orphan image assets (e.g. a replaced background) — drop
    // any img: asset nothing references, so it neither ships nor embeds. References
    // are found by scanning the SERIALIZED manifest rather than enumerating model
    // fields: image ids also appear in binding translationValues (PATH image swaps,
    // e.g. Elektrisk's drone lights) and any reference kind added later.
    {
        const auto json = dm::writeManifestToJson (library, true);
        juce::StringArray referenced;
        for (int pos = json.indexOf ("img:"); pos >= 0; pos = json.indexOf (pos + 1, "img:"))
        {
            int end = pos;
            while (end < json.length() && json[end] != '"' && json[end] != '\\')
                ++end;
            referenced.addIfNotAlreadyThere (json.substring (pos, end));
        }
        juce::StringArray orphaned;
        for (const auto& key : assets.getAllKeys())
            if (key.startsWith ("img:") && ! referenced.contains (key))
                orphaned.add (key);
        for (const auto& key : orphaned)
        {
            result.log.add ("drop  unreferenced image asset " + key);
            assets.remove (juce::StringRef (key));
        }
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
    // images sat directly in assets/). IR + images are a handful of small files, so they
    // are always rebuilt; SAMPLES (the bulk) are cleaned only when actually re-transcoded
    // (the incremental fast path below).
    for (auto& f : options.outDir.findChildFiles (juce::File::findFiles, false,
                                                  "*.flac;*.png;*.jpg;*.jpeg"))
        f.deleteFile();
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

    // Incremental fast path: samples are re-transcoded only when a source WAV changed.
    // A signature of every sample source (id|mtime|size) plus the packing mode is stored
    // next to the output; if it matches the previous run and the outputs are intact, the
    // whole transcode/pack stage is skipped and frame counts come from the cache (the pack
    // index for packed builds, the FLAC headers for loose). A manifest-only edit (strum
    // keys, colours, gains) then reconverts in a moment. --force always rebuilds.
    const auto sigFile = samplesDir.getChildFile (".cache-sig");
    juce::String sig = packing ? "pack\n" : "loose\n";
    for (auto& id : assets.getAllKeys())
        if (! id.startsWith ("img:") && ! id.startsWith ("ir:"))
        {
            const auto sf = options.libraryDir.getChildFile (assets[id]);
            sig << id << "|" << sf.getLastModificationTime().toMilliseconds()
                << "|" << sf.getSize() << "\n";
        }

    bool reuseSamples = ! options.forceRetranscode
                     && sigFile.existsAsFile()
                     && sigFile.loadFileAsString() == sig;
    if (reuseSamples)
    {
        if (packing)   // frames from the pack index (needs the "f" field written below)
        {
            reuseSamples = false;
            if (packFile.existsAsFile() && packIndexFile.existsAsFile())
            {
                const auto idxVar = juce::JSON::parse (packIndexFile.loadFileAsString());
                if (auto* arr = idxVar.getArray())   // named local: getArray() points INTO idxVar,
                {                                    // which must outlive the loop (else use-after-free)
                    bool ok = true;
                    for (auto& e : *arr)
                    {
                        auto* o = e.getDynamicObject();
                        if (o == nullptr || ! o->hasProperty ("f")) { ok = false; break; }
                        actualFrames.set (o->getProperty ("id").toString(), (juce::int64) o->getProperty ("f"));
                    }
                    reuseSamples = ok;
                }
            }
        }
        else   // loose FLACs: frames from each file's header
        {
            for (auto& id : assets.getAllKeys())
                if (! id.startsWith ("img:") && ! id.startsWith ("ir:"))
                {
                    const auto f = samplesDir.getChildFile (flacFileNameForId (id));
                    if (! f.existsAsFile()) { reuseSamples = false; break; }
                    actualFrames.set (id, flacFrameCount (f));
                }
        }
    }

    if (reuseSamples)
        result.log.add ("samples up to date - skipped re-transcode (--force to rebuild)");
    else
    {
        for (auto& f : samplesDir.findChildFiles (juce::File::findFiles, false, "*.flac"))
            f.deleteFile();
        packFile.deleteFile();
        packIndexFile.deleteFile();
    }

    std::unique_ptr<juce::FileOutputStream> packOut;
    juce::Array<juce::var> packEntries;
    if (packing && ! reuseSamples)
    {
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
        if (reuseSamples && ! isIr)   // sample already cached (frames set above)
            continue;

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
                    e->setProperty ("f", frames);   // frame count for the incremental cache
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

    // Remember this sample set so the next run can skip re-transcoding (only when the
    // build was clean — a failed transcode must not be cached as up to date).
    if (! reuseSamples && result.errors.isEmpty())
        sigFile.replaceWithText (sig, false, false, "\n");   // explicit LF: default CRLF would
                                                              // never match the LF-built sig on reload

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
            if (ui.strumSpeedReadout)
                ui.strumSpeedReadout->y += options.uiYOffset;
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

        // Keyboard-captions override (config "keyboardLabels"): same semantics as the
        // colours — a per-mode entry (or the default) REPLACES the mode's labels,
        // including any auto-derived by omnichordStrum.
        const auto lit = options.keyboardLabelsByMode.find (mode.name);
        if (lit != options.keyboardLabelsByMode.end() || options.haveKeyboardLabelsDefault)
        {
            const auto& labels = (lit != options.keyboardLabelsByMode.end())
                                     ? lit->second
                                     : options.keyboardLabelsDefault;
            mode.ui.keyboardLabels.clearQuick();
            for (const auto& kl : labels)
                mode.ui.keyboardLabels.add (kl);
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
