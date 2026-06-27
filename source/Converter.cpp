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

bool transcodeToFlac (const juce::File& wav, const juce::File& outFlac, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wav));
    if (reader == nullptr)
    {
        error = "cannot read " + wav.getFullPathName();
        return false;
    }

    outFlac.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (outFlac.createOutputStream());
    if (out == nullptr || ! out->openedOk())
    {
        error = "cannot write " + outFlac.getFullPathName();
        return false;
    }

    int bits = (int) reader->bitsPerSample;
    if (bits != 16 && bits != 24)
        bits = 24; // float or odd depths → 24-bit lossless

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

        juce::String error;
        if (transcodeToFlac (src, dst, error))
        {
            ++result.assetsTranscoded;
            result.log.add ("flac  " + dst.getFileName());
        }
        else
        {
            result.errors.add (error);
        }
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
