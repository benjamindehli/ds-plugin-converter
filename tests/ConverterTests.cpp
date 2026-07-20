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
        testOmnichordStrum();
        testBackgroundFromMode();
    }

    void testBackgroundFromMode()
    {
        beginTest ("backgroundFromMode borrows another mode's background and drops the orphan");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_bgfrom_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        auto outDir = root.getChildFile ("out");
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);
        for (const char* imgName : { "bgA.png", "bgB.png", "btn.png", "light_off.png", "light_on.png" })
        {
            auto f = libDir.getChildFile ("Resources").getChildFile (imgName);
            f.getParentDirectory().createDirectory();
            f.replaceWithText ("png-bytes");
        }

        auto preset = [] (const char* bg, const char* extra)
        {
            return juce::String (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui bgImage="Resources/)") + bg + R"(" width="800" height="300"><tab name="main">)" + extra + R"(</tab></ui>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group>
  </groups>
</DecentSampler>)";
        };
        // light_on.png is referenced ONLY via a PATH-binding translationValue (an
        // image swap, like Elektrisk's drone lights) — the orphan filter must keep it.
        const char* lamp = R"(
      <button x="10" y="10" width="20" height="20" style="image" value="0">
        <state name="off" mainImage="Resources/btn.png" hoverImage="Resources/btn.png" clickImage="Resources/btn.png">
          <binding type="control" level="ui" position="0" parameter="PATH" translation="fixed_value" translationValue="Resources/light_off.png"/>
        </state>
        <state name="on" mainImage="Resources/btn.png" hoverImage="Resources/btn.png" clickImage="Resources/btn.png">
          <binding type="control" level="ui" position="0" parameter="PATH" translation="fixed_value" translationValue="Resources/light_on.png"/>
        </state>
      </button>
      <image x="40" y="10" width="20" height="20" path="Resources/light_off.png"/>)";
        libDir.getChildFile ("A.dspreset").replaceWithText (preset ("bgA.png", lamp));
        libDir.getChildFile ("B.dspreset").replaceWithText (preset ("bgB.png", ""));

        dmconv::ConvertOptions opts;
        opts.packSamples = false;
        opts.libraryDir  = libDir;
        opts.outDir      = outDir;
        opts.libraryName = "BgLib";
        opts.backgroundFromMode["B"] = "A";

        auto result = dmconv::convertLibrary (opts);
        expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));

        auto m = dm::loadManifestFromFolder (outDir.getChildFile ("manifest"));
        expect (m.ok);
        expectEquals (m.library.modes.size(), 2);
        expectEquals (m.library.modes.getReference (0).ui.background, juce::String ("img:bgA"));
        expectEquals (m.library.modes.getReference (1).ui.background, juce::String ("img:bgA"));

        expect (outDir.getChildFile ("images/bgA.png").existsAsFile(), "shared background shipped");
        expect (! outDir.getChildFile ("images/bgB.png").existsAsFile(),
                "orphaned background neither ships nor embeds");
        expect (outDir.getChildFile ("images/light_on.png").existsAsFile(),
                "image referenced only by a PATH binding still ships");

        root.deleteRecursively();
    }

    void testOmnichordStrum()
    {
        beginTest ("omnichordStrum rewrites chord-order key-switches into strumKeys");

        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dmse_converter_omnistrum_test");
        root.deleteRecursively();
        root.createDirectory();

        auto libDir = root.getChildFile ("lib");
        writeSilentWav (libDir.getChildFile ("Samples/a.wav"), 1000);

        // A chord-order menu (options → SEQ_INDEX 0/1), two key-switches selecting
        // those options, and one chord trigger key.
        libDir.getChildFile ("Chords.dspreset").replaceWithText (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui width="812" height="375">
    <tab name="main">
      <menu x="124" y="120" width="162" height="22" value="1">
        <option name="Up"><binding type="note_binding" noteIndex="0" parameter="SEQ_INDEX" translation="fixed_value" translationValue="0"></binding></option>
        <option name="Down"><binding type="note_binding" noteIndex="0" parameter="SEQ_INDEX" translation="fixed_value" translationValue="1"></binding></option>
      </menu>
    </tab>
  </ui>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group>
  </groups>
  <noteSequences>
    <sequence name="Up" length="1" rate="1"><note position="0" velocity="1" note="60" length="1"></note></sequence>
    <sequence name="Down" length="1" rate="1"><note position="0" velocity="1" note="62" length="1"></note></sequence>
  </noteSequences>
  <midi>
    <note note="24" enabled="true">
      <binding type="control" level="ui" parameter="VALUE" controlIndex="0" translation="fixed_value" translationValue="1"></binding>
    </note>
    <note note="26" enabled="true">
      <binding type="control" level="ui" parameter="VALUE" controlIndex="0" translation="fixed_value" translationValue="2"></binding>
    </note>
    <note note="36" swallowNotes="true" enabled="true">
      <binding level="instrument" type="note_sequence" seqIndex="0" seqLoopMode="no_loop" seqTriggerBehavior="midi_key" seqTransposeWithRootNote="36" seqTrackMidiInputVelocity="1" seqPlaybackRate="10" enabled="true"></binding>
    </note>
  </midi>
</DecentSampler>)");

        dmconv::ConvertOptions opts;
        opts.packSamples = false;
        opts.libraryDir  = libDir;
        opts.libraryName = "OmniTest";

        // Default (off): key-switches stay key-switches, no strum keys. Recipe
        // "keyboardLabels" still applies (per-mode captions, e.g. the chord modes).
        {
            opts.keyboardLabelsByMode["Chords"] = { { 36, 47, "Major" }, { 48, 59, "Minor" } };
            opts.outDir = root.getChildFile ("out_plain");
            auto result = dmconv::convertLibrary (opts);
            expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));
            auto m = dm::loadManifestFromFolder (opts.outDir.getChildFile ("manifest"));
            expect (m.ok);
            const auto& mode = m.library.modes.getReference (0);
            expectEquals (mode.menuKeySwitches.size(), 2);
            expectEquals (mode.strumKeys.size(), 0);
            expectEquals (mode.ui.keyboardLabels.size(), 2);
            expectEquals (mode.ui.keyboardLabels.getReference (0).text, juce::String ("Major"));
            expectEquals (mode.ui.keyboardLabels.getReference (1).hiNote, 59);
            opts.keyboardLabelsByMode.clear();   // strum block below asserts the AUTO labels
        }

        // omnichordStrum: key-switches become strum keys with their option's offset,
        // the inert chord-order menu disappears, and keyboard labels are emitted
        // (strum-key captions from the recipe override + option-name fallback, and
        // a chord-type section from the sequence name).
        {
            opts.omnichordStrum = true;
            opts.strumKeyLabels.add ("A");   // key 24 override; key 26 falls back to "Down"
            opts.outDir = root.getChildFile ("out_strum");
            auto result = dmconv::convertLibrary (opts);
            expect (result.ok, "conversion should succeed: " + result.errors.joinIntoString ("; "));
            auto m = dm::loadManifestFromFolder (opts.outDir.getChildFile ("manifest"));
            expect (m.ok);
            const auto& mode = m.library.modes.getReference (0);
            expectEquals (mode.menuKeySwitches.size(), 0, "key-switches consumed");
            expectEquals (mode.sequenceTriggers.size(), 1, "chord triggers kept (as selectors)");
            expectEquals (mode.strumKeys.size(), 2);
            expectEquals (mode.strumKeys.getReference (0).note, 24);
            expectEquals (mode.strumKeys.getReference (0).seqOffset, 0);
            expectEquals (mode.strumKeys.getReference (1).note, 26);
            expectEquals (mode.strumKeys.getReference (1).seqOffset, 1);

            expect (mode.ui.tabs.getReference (0).menus.isEmpty(), "chord-order menu removed");
            expect (mode.ui.strumSpeedReadout.has_value(), "readout takes the menu's spot");
            expectEquals (mode.ui.strumSpeedReadout->x, 124);
            expectEquals (mode.ui.strumSpeedReadout->y, 120 + 50);   // + default uiYOffset
            expectEquals (mode.ui.strumSpeedReadout->width, 162);
            expectEquals (mode.ui.strumSpeedReadout->height, 22);
            const auto& labels = mode.ui.keyboardLabels;
            expectEquals (labels.size(), 3);
            expectEquals (labels.getReference (0).loNote, 24);
            expectEquals (labels.getReference (0).text, juce::String ("A"));
            expectEquals (labels.getReference (1).loNote, 26);
            expectEquals (labels.getReference (1).text, juce::String ("Down"));
            expectEquals (labels.getReference (2).loNote, 36);
            expectEquals (labels.getReference (2).hiNote, 36);
            expectEquals (labels.getReference (2).text, juce::String ("Up"));   // seq name, no root prefix to strip
        }

        root.deleteRecursively();
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
