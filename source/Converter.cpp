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

    // 3. Transcode assets → FLAC in the output dir.
    if (auto dirResult = options.outDir.createDirectory(); dirResult.failed())
    {
        result.errors.add ("cannot create out dir: " + dirResult.getErrorMessage());
        return result;
    }

    for (auto& id : assets.getAllKeys())
    {
        const auto src = options.libraryDir.getChildFile (assets[id]);
        const auto dst = options.outDir.getChildFile (flacFileNameForId (id));

        if (! src.existsAsFile())
        {
            result.errors.add ("missing source asset: " + src.getFullPathName());
            continue;
        }

        juce::int64 frames = 0;
        juce::String formatNote, error;
        if (transcodeToFlac (src, dst, frames, formatNote, error))
        {
            ++result.assetsTranscoded;
            result.log.add ("flac  " + dst.getFileName());

            if (formatNote.isNotEmpty())
                result.warnings.add (formatNote);

            actualFrames.set (id, frames);
        }
        else
        {
            result.errors.add (error);
        }
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

    // 4. Write the manifest.
    result.manifestFile = options.outDir.getChildFile ("manifest.json");
    const auto json = dm::writeManifestToJson (library);
    if (! result.manifestFile.replaceWithText (json))
        result.errors.add ("cannot write manifest: " + result.manifestFile.getFullPathName());

    result.modes = library.modes.size();
    result.ok = result.errors.isEmpty();
    return result;
}

} // namespace dmconv
