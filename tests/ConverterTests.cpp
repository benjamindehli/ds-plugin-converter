// Integration tests for the converter, using throwaway synthesized libraries:
//  - length is taken from the audio (mismatch summarised, audio untouched)
//  - --reverb-gain bakes outputLevel (dB) into convolution effects

#include "../source/Converter.h"
#include <model/ManifestLoader.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace
{
void writeSilentWav (const juce::File& file, int frames)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (out.get(), 48000.0, 1, 16, {}, 0));
    if (writer != nullptr)
    {
        out.release();
        juce::AudioBuffer<float> buf (1, frames);
        buf.clear();
        writer->writeFromAudioSampleBuffer (buf, 0, frames);
        writer.reset();
    }
}

bool anyWarningContains (const juce::StringArray& warnings, const juce::String& needle)
{
    for (auto& w : warnings)
        if (w.contains (needle))
            return true;
    return false;
}

class ConverterTests : public juce::UnitTest
{
public:
    ConverterTests() : juce::UnitTest ("Converter", "converter") {}

    void runTest() override
    {
        testLengthOverride();
        testReverbGain();
        testLoopValidation();
        testImageEmbedding();
        testUiYOffset();
        testSplitManifestOutput();
        testSamplePack();
    }

    void testSplitManifestOutput()
    {
        beginTest ("split manifest/ folder is the sole manifest output and round-trips");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_split_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);

        // Two presets → two modes, exercising per-mode file splitting + the index list.
        const juce::String preset (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group>
  </groups>
</DecentSampler>)");
        libDir.getChildFile ("Bass.dspreset").replaceWithText (preset);
        libDir.getChildFile ("Keys.dspreset").replaceWithText (preset);

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir  = libDir;
        opts.outDir      = outDir;
        opts.libraryName = "TestLib";

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));

        auto manifestDir = outDir.getChildFile ("manifest");
        expect (manifestDir.getChildFile ("index.json").existsAsFile(), "index.json written");
        expectEquals (manifestDir.getChildFile ("modes")
                          .getNumberOfChildFiles (juce::File::findFiles, "*.json"),
                      2, "one file per mode");

        // Split-only output: no single manifest.json is written any more.
        expect (! outDir.getChildFile ("manifest.json").existsAsFile(), "no single manifest.json emitted");

        // The split folder reloads into a full 2-mode library.
        auto split = dm::loadManifestFromFolder (manifestDir);
        expect (split.ok, "split manifest reloads: " + split.errors.joinIntoString ("; "));
        expectEquals (split.library.modes.size(), 2);
        for (const auto& m : split.library.modes)
            expect (! m.groups.isEmpty() && ! m.groups.getReference (0).samples.isEmpty(),
                    "each mode carries its samples through the split round-trip");

        root.deleteRecursively();
    }

    void testUiYOffset()
    {
        beginTest ("UI element y shifted by the menu-bar offset");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_uiy_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);

        // A control with no skin → no image asset to resolve.
        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui width="812" height="375">
    <tab name="main">
      <control x="10" y="10" width="49" height="49" parameterName="K" type="float" minValue="0" maxValue="1" value="0"/>
    </tab>
  </ui>
  <groups><group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group></groups>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir = libDir;
        opts.outDir     = outDir;
        // opts.uiYOffset defaults to 100

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, result.errors.joinIntoString ("; "));

        auto reloaded = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (reloaded.ok);
        const auto& tab = reloaded.library.modes.getReference (0).ui.tabs.getReference (0);
        expectEquals (tab.controls.size(), 1);
        expectEquals (tab.controls.getReference (0).rect.y, 60);    // 10 + 50 offset
        expectEquals (tab.controls.getReference (0).rect.x, 10);    // x unchanged
    }

    void testImageEmbedding()
    {
        beginTest ("UI images embedded verbatim (not transcoded)");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_img_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");

        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);
        auto bg = libDir.getChildFile ("Resources/bg.png");
        bg.getParentDirectory().createDirectory();   // replaceWithText won't create dirs
        bg.replaceWithText ("not-really-a-png-but-bytes");

        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui bgImage="Resources/bg.png" width="800" height="300">
    <tab name="main"/>
  </ui>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group>
  </groups>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir  = libDir;
        opts.outDir      = outDir;
        opts.libraryName = "TestLib";

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));

        expect (outDir.getChildFile ("images/bg.png").existsAsFile(), "image copied verbatim");
        expect (! outDir.getChildFile ("images/bg.flac").existsAsFile(), "image must not be transcoded");
        expect (outDir.getChildFile ("samples/a.flac").existsAsFile(), "audio still transcoded");

        // Byte-for-byte identical (no modification).
        expectEquals (outDir.getChildFile ("images/bg.png").loadFileAsString(),
                      juce::String ("not-really-a-png-but-bytes"));

        auto reloaded = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (reloaded.ok);
        expectEquals (reloaded.library.modes.getReference (0).ui.background, juce::String ("img:bg"));
    }

    void testLengthOverride()
    {
        beginTest ("audio length is authoritative; mismatch is summarised, audio untouched");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");

        // a.wav: 1000 frames, declared 1000 (matches). b.wav: 500 frames, declared 999.
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);
        writeSilentWav (libDir.getChildFile ("Samples/b.wav"), 500);

        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group>
      <sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/>
      <sample path="Samples/b.wav" loNote="61" hiNote="61" rootNote="61" length="999"  sampleRate="48000"/>
    </group>
  </groups>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir = libDir;
        opts.outDir     = outDir;
        opts.libraryName = "TestLib";

        auto result = dmconv::convertLibrary (opts);

        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));
        expectEquals (result.assetsTranscoded, 2);
        expect (outDir.getChildFile ("manifest/index.json").existsAsFile(), "split manifest written");
        expect (! outDir.getChildFile ("manifest.json").existsAsFile(), "no single manifest.json emitted");
        expect (outDir.getChildFile ("samples/a.flac").existsAsFile(), "matching file still transcoded");
        expect (outDir.getChildFile ("samples/b.flac").existsAsFile(), "mismatching file still transcoded");

        // b's declared length (999) disagrees with its real length (500) → summarised.
        expect (anyWarningContains (result.warnings, "b.flac"), "expected the override example to cite b.flac");
        expect (anyWarningContains (result.warnings, "500"));
        expect (anyWarningContains (result.warnings, "999"));
        expect (! anyWarningContains (result.warnings, "a.flac"),
                "matching file should not appear in the override summary");

        // The manifest now carries the ACTUAL decoded lengths, not the .dspreset's.
        auto reloaded = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (reloaded.ok, "generated manifest should reload");
        int lenA = -1, lenB = -1;
        for (const auto& g : reloaded.library.modes.getReference (0).groups)
            for (const auto& s : g.samples)
            {
                if (s.source == "flac:a" && s.lengthFrames) lenA = *s.lengthFrames;
                if (s.source == "flac:b" && s.lengthFrames) lenB = *s.lengthFrames;
            }
        expectEquals (lenA, 1000); // matched declaration, unchanged
        expectEquals (lenB, 500);  // overridden from the wrong 999

        root.deleteRecursively();
    }

    void testReverbGain()
    {
        beginTest ("--reverb-gain bakes outputLevel into convolution effects only");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_reverb_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");

        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);
        writeSilentWav (libDir.getChildFile ("IR/space.wav"), 200);

        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group>
      <sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/>
    </group>
  </groups>
  <effects>
    <effect type="lowpass" frequency="15000" enabled="false"/>
    <effect type="convolution" irFile="IR/space.wav" wetLevel="0.3"/>
  </effects>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir   = libDir;
        opts.outDir       = outDir;
        opts.libraryName  = "TestLib";
        opts.reverbGainDb = 12.0;

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));

        auto reloaded = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (reloaded.ok, "generated manifest should reload");

        const auto& effects = reloaded.library.modes.getReference (0).effects;
        bool checkedConvolution = false, checkedLowpass = false;
        for (const auto& e : effects)
        {
            if (e.type == "convolution")
            {
                checkedConvolution = true;
                expect (e.outputLevel.has_value(), "convolution should carry baked outputLevel");
                expectWithinAbsoluteError (e.outputLevel.value_or (0.0), 12.0, 1.0e-6);
                expect (e.wet.has_value() && std::abs (*e.wet - 0.3) < 1.0e-6, "wet preserved");
            }
            if (e.type == "lowpass")
            {
                checkedLowpass = true;
                expect (! e.outputLevel.has_value(), "reverb gain must not touch the lowpass");
            }
        }
        expect (checkedConvolution, "expected a convolution effect");
        expect (checkedLowpass, "expected a lowpass effect");

        root.deleteRecursively();
    }

    void testLoopValidation()
    {
        beginTest ("out-of-range loops disabled (warn-only, audio untouched)");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_loop_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");

        writeSilentWav (libDir.getChildFile ("Samples/long.wav"), 1000);
        writeSilentWav (libDir.getChildFile ("Samples/short.wav"), 1000);

        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group>
      <sample path="Samples/long.wav"  loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000" loopEnabled="1" loopStart="100" loopEnd="900"/>
      <sample path="Samples/short.wav" loNote="61" hiNote="61" rootNote="61" length="1000" sampleRate="48000" loopEnabled="1" loopStart="100" loopEnd="99999"/>
    </group>
  </groups>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;   // these tests assert on loose per-sample FLACs
        opts.libraryDir  = libDir;
        opts.outDir      = outDir;
        opts.libraryName = "TestLib";

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));

        auto reloaded = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (reloaded.ok);

        bool checkedLong = false, checkedShort = false;
        for (const auto& g : reloaded.library.modes.getReference (0).groups)
            for (const auto& s : g.samples)
            {
                if (s.source == "flac:long")
                {
                    checkedLong = true;
                    expect (s.loop.enabled, "valid loop should be kept");
                }
                if (s.source == "flac:short")
                {
                    checkedShort = true;
                    expect (! s.loop.enabled, "out-of-range loop should be disabled");
                }
            }
        expect (checkedLong && checkedShort);
        expect (anyWarningContains (result.warnings, "short.flac"), "expected a loop warning for short.flac");

        root.deleteRecursively();
    }

    void testSamplePack()
    {
        beginTest ("default pack mode emits samples.pak + index instead of loose FLACs");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_pack_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);

        libDir.getChildFile ("Kit.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group>
      <sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/>
    </group>
  </groups>
</DecentSampler>)");

        dmconv::ConvertOptions opts;   // packSamples defaults TRUE — that is the point
        opts.libraryDir = libDir;
        opts.outDir     = outDir;

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "pack conversion should succeed: " + result.errors.joinIntoString ("; "));

        expect (! outDir.getChildFile ("samples/a.flac").existsAsFile(),
                "packed mode must not leave loose sample FLACs");
        expect (outDir.getChildFile ("samples/samples.pak").existsAsFile(), "pack written");
        expect (outDir.getChildFile ("samples/samples.pak.json").existsAsFile(), "pack index written");

        juce::var idx;
        expect (juce::JSON::parse (outDir.getChildFile ("samples/samples.pak.json").loadFileAsString(),
                                   idx).wasOk(), "pack index parses");
        auto* entries = idx.getArray();
        expect (entries != nullptr && entries->size() == 1, "one pack entry");
        if (entries != nullptr && ! entries->isEmpty())
        {
            const auto& e = entries->getReference (0);
            expectEquals (e.getProperty ("id", "").toString(), juce::String ("flac:a"));
            const int off = (int) e.getProperty ("o", -1);
            const int len = (int) e.getProperty ("l", -1);
            expect (off >= 0 && len > 0, "entry has offset+length");
            expect ((juce::int64) (off + len) <= outDir.getChildFile ("samples/samples.pak").getSize(),
                    "entry fits inside the pack");
        }

        root.deleteRecursively();
    }

};

ConverterTests converterTests;
} // namespace
