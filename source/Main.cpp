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

// Populate options from a JSON config file (the per-library recipe), e.g.
//   { "gain": -16, "reverbGain": -16, "normalizeIr": false,
//     "uiYOffset": 50, "cropTop": { "default": 60, "Split": 0 } }
// CLI flags applied afterwards override these. Returns false if the file is absent
// (fine — config is optional) or malformed (reported by the caller).
std::vector<dm::KeyboardColor> parseKeyboardColors (const juce::var& arr)
{
    std::vector<dm::KeyboardColor> out;
    if (auto* a = arr.getArray())
        for (const auto& e : *a)
            if (auto* o = e.getDynamicObject())
            {
                dm::KeyboardColor kc;
                kc.loNote = (int) o->getProperty ("loNote");
                kc.hiNote = o->hasProperty ("hiNote") ? (int) o->getProperty ("hiNote") : 127;
                kc.color  = o->getProperty ("color").toString();
                out.push_back (kc);
            }
    return out;
}

bool applyConfigFile (const juce::File& cfg, dmconv::ConvertOptions& opts, juce::String& error)
{
    if (! cfg.existsAsFile())
        return true;   // no config is fine

    juce::var v;
    if (juce::JSON::parse (cfg.loadFileAsString(), v).failed() || ! v.isObject())
    {
        error = "config file is not valid JSON: " + cfg.getFullPathName();
        return false;
    }

    auto* o = v.getDynamicObject();
    if (o->hasProperty ("gain"))        opts.gainDb       = (double) o->getProperty ("gain");
    if (o->hasProperty ("reverbGain"))  opts.reverbGainDb = (double) o->getProperty ("reverbGain");
    if (o->hasProperty ("normalizeIr")) opts.normalizeIr  = (bool)   o->getProperty ("normalizeIr");
    if (o->hasProperty ("packSamples")) opts.packSamples  = (bool)   o->getProperty ("packSamples");
    if (o->hasProperty ("polySaveDefault")) opts.polySaveDefault = (bool) o->getProperty ("polySaveDefault");
    if (o->hasProperty ("whiteKeyTint")) opts.whiteKeyTint = o->getProperty ("whiteKeyTint").toString();
    if (o->hasProperty ("blackKeyTint")) opts.blackKeyTint = o->getProperty ("blackKeyTint").toString();
    if (auto* dt = o->getProperty ("dropGroupTags").getArray())
        for (const auto& t : *dt) opts.dropGroupTags.add (t.toString());
    if (o->hasProperty ("doubleTrackBoostTag")) opts.doubleTrackBoostTag = o->getProperty ("doubleTrackBoostTag").toString();
    if (o->hasProperty ("doubleTrackStereoBoost")) opts.doubleTrackStereoBoost = (double) o->getProperty ("doubleTrackStereoBoost");
    if (o->hasProperty ("uiYOffset"))   opts.uiYOffset    = (int)    o->getProperty ("uiYOffset");

    const auto crop = o->getProperty ("cropTop");
    if (auto* co = crop.getDynamicObject())          // { "default": N, "Name": N, ... }
    {
        for (const auto& p : co->getProperties())
        {
            const auto key = p.name.toString();
            const int  px  = (int) p.value;
            if (key == "*" || key == "default") opts.cropTopDefault = px;
            else                                opts.cropTopByMode[key] = px;
        }
    }
    else if (! crop.isVoid())                         // scalar → default for all modes
        opts.cropTopDefault = (int) crop;

    const auto kb = o->getProperty ("keyboardColors");
    if (kb.isArray())                                 // flat array → applies to every mode
    {
        opts.keyboardColorsDefault = parseKeyboardColors (kb);
        opts.haveKeyboardDefault   = true;
    }
    else if (auto* kbo = kb.getDynamicObject())       // { "default": [...], "ModeName": [...] }
    {
        for (const auto& p : kbo->getProperties())
        {
            const auto key = p.name.toString();
            if (key == "*" || key == "default")
            {
                opts.keyboardColorsDefault = parseKeyboardColors (p.value);
                opts.haveKeyboardDefault   = true;
            }
            else
                opts.keyboardColorsByMode[key] = parseKeyboardColors (p.value);
        }
    }

    return true;
}
}

int main (int argc, char* argv[])
{
    juce::StringArray positional;
    std::optional<double> reverbGainDb;
    std::optional<double> gainDb;
    std::optional<bool> normalizeIr;
    std::optional<bool> packSamples;
    std::optional<int> uiYOffset;
    juce::String cropTopSpec;
    juce::String configArg;

    for (int i = 1; i < argc; ++i)
    {
        const auto arg = juce::String::fromUTF8 (argv[i]);
        if (arg == "--reverb-gain" && i + 1 < argc)
            reverbGainDb = juce::String::fromUTF8 (argv[++i]).getDoubleValue();
        else if (arg == "--gain" && i + 1 < argc)
            gainDb = juce::String::fromUTF8 (argv[++i]).getDoubleValue();
        else if (arg == "--no-normalize-ir")
            normalizeIr = false;
        else if (arg == "--pack-samples")
            packSamples = true;
        else if (arg == "--no-pack-samples")
            packSamples = false;
        else if (arg == "--ui-y-offset" && i + 1 < argc)
            uiYOffset = juce::String::fromUTF8 (argv[++i]).getIntValue();
        else if (arg == "--crop-top" && i + 1 < argc)
            cropTopSpec = juce::String::fromUTF8 (argv[++i]);
        else if (arg == "--config" && i + 1 < argc)
            configArg = juce::String::fromUTF8 (argv[++i]);
        else
            positional.add (arg);
    }

    if (positional.size() < 2)
    {
        std::cout << "usage: dmse-convert <library-dir> <out-dir> [preset ...] [--reverb-gain <dB>] [--gain <dB>]\n"
                  << "  Converts DecentSampler .dspreset presets into the engine\n"
                  << "  JSON manifest + FLAC bundle in <out-dir>.\n"
                  << "  --reverb-gain <dB>   trim every convolution wet (e.g. 12 for Omni-84).\n"
                  << "  --gain <dB>          library-wide pre-FX level trim (e.g. -10 for Midnight Wurli).\n"
                  << "  --no-normalize-ir    use convolution IRs as recorded (like DS; for cabinet IRs).\n"
                  << "  --no-pack-samples    embed sample FLACs in the binary instead of the default\n"
                  << "                       disk pack (samples/samples.pak + .json index that the plugin\n"
                  << "                       memory-maps). Packing is ON by default (small binary, fast\n"
                  << "                       launch); use this to make a fully self-contained build.\n"
                  << "  --ui-y-offset <px>   shift UI elements down (default 100; menu-bar offset).\n"
                  << "  --crop-top <spec>    trim dead header space per mode. <spec> is comma-separated:\n"
                  << "                       a bare number = default for all modes; NAME=px overrides one\n"
                  << "                       mode (preset name). e.g. --crop-top \"60,Split=0\".\n"
                  << "  --config <file>      read options from a JSON recipe (default: <library-dir>/\n"
                  << "                       dmse-convert.json if present). CLI flags override it.\n";
        return 2;
    }

    dmconv::ConvertOptions opts;
    opts.libraryDir   = resolvePath (positional[0]);
    opts.outDir       = resolvePath (positional[1]);
    for (int i = 2; i < positional.size(); ++i)
        opts.presetFilter.add (positional[i]);

    // Config file (the per-library recipe): explicit --config, else auto-discover
    // <library-dir>/dmse-convert.json. Provides defaults; CLI flags below override.
    const juce::File configFile = configArg.isNotEmpty() ? resolvePath (configArg)
                                                         : opts.libraryDir.getChildFile ("dmse-convert.json");
    if (juce::String cfgError; ! applyConfigFile (configFile, opts, cfgError))
    {
        std::cout << "error: " << cfgError << "\n";
        return 2;
    }

    // CLI flags override the config where the user passed them.
    if (reverbGainDb.has_value()) opts.reverbGainDb = reverbGainDb;
    if (gainDb.has_value())       opts.gainDb       = gainDb;
    if (normalizeIr.has_value())  opts.normalizeIr  = *normalizeIr;
    if (packSamples.has_value())  opts.packSamples  = *packSamples;
    if (uiYOffset.has_value())    opts.uiYOffset    = *uiYOffset;

    // --crop-top spec REPLACES the config's crop: bare number = default for all modes;
    // NAME=px overrides one mode.
    if (cropTopSpec.isNotEmpty())
    {
        opts.cropTopDefault = 0;
        opts.cropTopByMode.clear();
        for (auto token : juce::StringArray::fromTokens (cropTopSpec, ",", ""))
        {
            token = token.trim();
            if (token.isEmpty())
                continue;
            if (token.containsChar ('='))
                opts.cropTopByMode[token.upToFirstOccurrenceOf ("=", false, false).trim()]
                    = token.fromFirstOccurrenceOf ("=", false, false).trim().getIntValue();
            else
                opts.cropTopDefault = token.getIntValue();
        }
    }

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
