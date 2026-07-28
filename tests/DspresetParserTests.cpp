// Unit tests for the .dspreset parser (B2).
//
// Parses the fixture presets (no WAVs needed — parsing is pure on XML text) and
// asserts the mapped engine model, the collected asset table, and a round-trip
// through the engine's ManifestWriter/loader. Built as its own console app.

#include "../source/DspresetParser.h"
#include <model/ManifestWriter.h>
#include <model/ManifestLoader.h>
#include <juce_core/juce_core.h>
#include <iostream>

namespace
{
juce::File fixturesDir()
{
   #if defined (DMSE_CONVERTER_FIXTURES_DIR)
    return juce::File (DMSE_CONVERTER_FIXTURES_DIR);
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/fixtures");
   #endif
}

class DspresetParserTests : public juce::UnitTest
{
public:
    DspresetParserTests() : juce::UnitTest ("DspresetParser", "converter") {}

    juce::String load (const juce::String& lib, const juce::String& file)
    {
        auto f = fixturesDir().getChildFile (lib).getChildFile (file);
        if (! f.existsAsFile())
            expect (false, "fixture not found: " + f.getFullPathName());
        return f.loadFileAsString();
    }

    void runTest() override
    {
        testBass();
        testMaskinTrommer();
        testReleaseTriggerAndLfo();
        testSequenceTriggers();
        testMenu();
        testGroupEffectBindings();
        testElektriskIdBindings();
        testSteppedControl();
        testRoundTrip();
    }

    // DecentSampler valueType="integer" → the control is a stepped knob; type="float" is not.
    void testSteppedControl()
    {
        beginTest ("valueType=\"integer\" marks a control stepped");
        auto r = dmconv::parseDspreset (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui width="400" height="200"><tab name="main">
    <control x="10" y="10" width="40" height="40" parameterName="Sustain" minValue="0" maxValue="6" valueType="integer" value="0"/>
    <control x="60" y="10" width="40" height="40" parameterName="Body" type="float" minValue="0" maxValue="10" value="5"/>
  </tab></ui>
  <groups><group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60"/></group></groups>
</DecentSampler>)", "M");
        expect (r.ok, r.errors.joinIntoString ("; "));
        const auto& controls = r.mode.ui.tabs.getReference (0).controls;
        expectEquals (controls.size(), 2);
        expect (controls.getReference (0).stepped,   "integer control should be stepped");
        expect (! controls.getReference (1).stepped, "float control should not be stepped");
    }

    // The real Elektrisk Salmesykkel presets are the production stress test for
    // id-based bindings: seven organ-stop groups, each with a per-group swell lowpass
    // and loudness gain, driven through 28 group-effect bindings plus UI cross-refs.
    // Parsing must resolve every positional index to a targetId — the parser's own
    // validation fails the conversion otherwise, so r.ok proves the migration is
    // complete for these presets (the ones I cannot re-convert without the samples).
    void testElektriskIdBindings()
    {
        beginTest ("Elektrisk presets: every binding resolves to an id (no bare index)");

        for (const auto* file : { "ElektriskSalmesykkel.dspreset",
                                  "ElektriskSalmesykkel (Looped).dspreset" })
        {
            auto r = dmconv::parseDspreset (load ("elektrisk-salmesykkel", file), "Organ");
            expect (r.ok, juce::String (file) + ": " + r.errors.joinIntoString ("; "));

            int groupEffectBindings = 0;
            auto audit = [&] (const dm::Binding& b)
            {
                const bool hasIndex = b.effectIndex.has_value() || b.groupIndex.has_value()
                                    || b.controlIndex.has_value() || b.position.has_value();
                if (hasIndex)
                    expect (b.targetId.isNotEmpty(),
                            juce::String (file) + ": binding with an index kept no targetId ("
                            + b.parameter + ")");
                // Group-effect binding → targetId names a per-group effect (…_fx_…).
                if (b.level == "group" && b.type == "effect")
                {
                    ++groupEffectBindings;
                    expect (b.targetId.contains ("_fx_"),
                            juce::String (file) + ": group-effect targetId not an effect id: " + b.targetId);
                }
            };
            for (auto& tab : r.mode.ui.tabs)
            {
                for (auto& c  : tab.controls) for (auto& b : c.bindings) audit (b);
                for (auto& bt : tab.buttons)  for (auto& st : bt.states)  for (auto& b : st.bindings) audit (b);
                for (auto& mn : tab.menus)    for (auto& op : mn.options)  for (auto& b : op.bindings) audit (b);
            }
            for (auto& lfo : r.mode.modulators)
                for (auto& b : lfo.bindings) audit (b);

            expect (groupEffectBindings > 0, juce::String (file) + ": expected group-effect bindings");
        }
    }

    // A group-effect binding (level="group" + groupIndex + effectIndex, as in the
    // Elektrisk organ swell) must be rewritten to reference the specific per-group
    // effect by its own id, so the (group, slot) target no longer rides on positional
    // indices. Mirrors the real preset: each group has a swell lowpass in slot 0 and a
    // loudness gain in slot 1, driven by a single fader control.
    void testGroupEffectBindings()
    {
        beginTest ("group-effect binding → per-group effect id (targetId)");
        auto r = dmconv::parseDspreset (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui width="812" height="375">
    <tab name="main">
      <control x="98" y="39" width="58" height="120" parameterName="Loudness" minValue="0" maxValue="127" value="0">
        <binding type="effect" level="group" groupIndex="0" effectIndex="0" parameter="FX_FILTER_FREQUENCY" translation="linear" translationOutputMin="4000" translationOutputMax="22000"/>
        <binding type="effect" level="group" groupIndex="1" effectIndex="1" parameter="LEVEL" translation="linear" translationOutputMin="0.0" translationOutputMax="6.0"/>
      </control>
    </tab>
  </ui>
  <groups>
    <group>
      <effects>
        <effect type="lowpass" frequency="15000"/>
        <effect type="gain" level="0"/>
      </effects>
      <sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60"/>
    </group>
    <group>
      <effects>
        <effect type="lowpass" frequency="15000"/>
        <effect type="gain" level="0"/>
      </effects>
      <sample path="Samples/b.wav" loNote="62" hiNote="62" rootNote="62"/>
    </group>
  </groups>
</DecentSampler>)", "Organ");

        expect (r.ok, r.errors.joinIntoString ("; "));
        expectEquals (r.mode.groups.size(), 2);

        // Each per-group effect got a stable id scoped by its group's uid.
        const auto& g0 = r.mode.groups.getReference (0);
        const auto& g1 = r.mode.groups.getReference (1);
        expectEquals (g0.effects.size(), 2);
        expectEquals (g0.effects.getReference (0).id, juce::String ("grp_0_fx_lowpass"));
        expectEquals (g0.effects.getReference (1).id, juce::String ("grp_0_fx_gain"));
        expectEquals (g1.effects.getReference (1).id, juce::String ("grp_1_fx_gain"));

        // The bindings now point at the specific effect by id (group 0 slot 0 filter,
        // group 1 slot 1 gain), not at the group + a positional effectIndex.
        const auto& binds = r.mode.ui.tabs.getReference (0).controls.getReference (0).bindings;
        expectEquals (binds.size(), 2);
        expectEquals (binds.getReference (0).targetId, juce::String ("grp_0_fx_lowpass"));
        expectEquals (binds.getReference (1).targetId, juce::String ("grp_1_fx_gain"));
    }

    void testMenu()
    {
        beginTest ("<menu> → Menu with per-option SEQ_INDEX (no offset in parser)");
        auto r = dmconv::parseDspreset (R"(<?xml version="1.0"?>
<DecentSampler>
  <ui width="812" height="375">
    <tab name="main">
      <menu x="124" y="120" width="162" height="22" value="1">
        <option name="A"><binding type="note_binding" noteIndex="0" parameter="SEQ_INDEX" translation="fixed_value" translationValue="0"></binding></option>
        <option name="B"><binding type="note_binding" noteIndex="0" parameter="SEQ_INDEX" translation="fixed_value" translationValue="84"></binding></option>
      </menu>
    </tab>
  </ui>
  <groups><group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60"/></group></groups>
</DecentSampler>)", "M");

        expect (r.ok, r.errors.joinIntoString ("; "));
        expect (! r.mode.ui.tabs.isEmpty());
        const auto& tab = r.mode.ui.tabs.getReference (0);
        expectEquals (tab.menus.size(), 1);
        const auto& menu = tab.menus.getReference (0);
        expectEquals (menu.value, 1);
        expectEquals (menu.options.size(), 2);
        expectEquals (menu.options.getReference (0).seqIndex, 0);
        expectEquals (menu.options.getReference (1).seqIndex, 84);
        expectEquals (menu.rect.y, 120);   // parser leaves y as-authored
    }

    void testSequenceTriggers()
    {
        beginTest ("<midi> note_sequence bindings → sequenceTriggers");

        auto r = dmconv::parseDspreset (R"(<?xml version="1.0"?>
<DecentSampler>
  <groups attack="0" decay="0" sustain="1" release="0.1">
    <group><sample path="Samples/a.wav" loNote="60" hiNote="60" rootNote="60" length="1000" sampleRate="48000"/></group>
  </groups>
  <noteSequences>
    <sequence name="One" length="1" rate="1"><note position="0" velocity="1" note="60" length="1"></note></sequence>
    <sequence name="Two" length="1" rate="1"><note position="0" velocity="1" note="62" length="1"></note></sequence>
  </noteSequences>
  <midi>
    <note note="36" swallowNotes="true" enabled="true">
      <binding level="instrument" type="note_sequence" seqIndex="0" seqLoopMode="no_loop" seqTriggerBehavior="midi_key" seqTransposeWithRootNote="36" seqTrackMidiInputVelocity="1" seqPlaybackRate="10" enabled="true"></binding>
    </note>
    <note note="37" swallowNotes="true" enabled="true">
      <binding level="instrument" type="note_sequence" seqIndex="1" seqLoopMode="no_loop" seqTriggerBehavior="midi_key" seqTransposeWithRootNote="37" seqTrackMidiInputVelocity="1" seqPlaybackRate="10" enabled="true"></binding>
    </note>
  </midi>
</DecentSampler>)", "Seq");

        expect (r.ok, "should parse: " + r.errors.joinIntoString ("; "));
        expectEquals (r.mode.sequences.size(), 2);
        expectEquals (r.mode.sequenceTriggers.size(), 2);

        const auto& t0 = r.mode.sequenceTriggers.getReference (0);
        expectEquals (t0.note, 36);
        expectEquals (t0.sequence, 0);
        expectEquals (t0.transpose, 0);
        expect (t0.swallow);
        expect (t0.trackVelocity);
        expect (! t0.loop);
        expectWithinAbsoluteError (t0.rate, 10.0, 1.0e-9);

        const auto& t1 = r.mode.sequenceTriggers.getReference (1);
        expectEquals (t1.note, 37);
        expectEquals (t1.sequence, 1);
    }

    void testBass()
    {
        beginTest ("Bass.dspreset → model");
        auto r = dmconv::parseDspreset (load ("omni-84", "Bass.dspreset"), "Bass");
        expect (r.ok, "Bass should parse: " + r.errors.joinIntoString ("; "));
        expectEquals (r.mode.name, juce::String ("Bass"));

        // amp envelope from <groups>
        expectWithinAbsoluteError (r.mode.amp.release, 0.1, 1.0e-9);
        expectWithinAbsoluteError (r.mode.amp.sustain, 1.0, 1.0e-9);

        // one group, 19 samples (notes 24..42), fixed pitch, no loop
        expectEquals (r.mode.groups.size(), 1);
        const auto& g = r.mode.groups.getReference (0);
        expectEquals (g.samples.size(), 19);
        const auto& s0 = g.samples.getReference (0);
        expectEquals (s0.source, juce::String ("flac:Bass_0C"));
        expectEquals (s0.rootNote, 24);
        expect (! s0.pitchKeyTrack);
        expect (! s0.loop.enabled);
        expect (s0.lengthFrames.has_value() && s0.lengthFrames.value() == 805888);

        // silencing / tags
        expect (g.tags.contains ("monophonic"));
        expect (g.silencing.has_value());
        expectEquals (g.silencing->mode, juce::String ("normal"));
        expect (g.silencing->byTags.contains ("monophonic"));
        expectEquals (r.mode.tags.size(), 1);
        expect (r.mode.tags.getReference (0).polyphony.value_or (0) == 1);

        // effects: disabled lowpass + convolution IR
        expectEquals (r.mode.effects.size(), 2);
        expectEquals (r.mode.effects.getReference (0).type, juce::String ("lowpass"));
        expect (! r.mode.effects.getReference (0).enabled);
        expectEquals (r.mode.effects.getReference (1).type, juce::String ("convolution"));
        expectEquals (r.mode.effects.getReference (1).ir, juce::String ("ir:Space"));

        // asset table (id → library-relative path)
        expectEquals (r.assets ["flac:Bass_0C"], juce::String ("Samples/Bass/Bass_0C.wav"));
        expectEquals (r.assets ["ir:Space"], juce::String ("IR/Space.wav"));

        // UI tree + image assets
        expectEquals (r.mode.ui.background, juce::String ("img:background_bass"));
        expect (r.mode.ui.tabs.size() >= 1);
        const auto& tab = r.mode.ui.tabs.getReference (0);
        expectEquals (tab.controls.size(), 3);
        const auto& c0 = tab.controls.getReference (0);
        expectEquals (c0.label, juce::String ("Sustain"));
        expect (c0.skin.has_value());
        expectEquals (c0.skin->image, juce::String ("img:Knob"));
        expect (c0.skin->numFrames.value_or (0) == 101);
        expect (! c0.bindings.isEmpty());
        expectEquals (c0.bindings.getReference (0).parameter, juce::String ("ENV_RELEASE"));
        expectEquals (tab.buttons.size(), 1);
        expectEquals (tab.buttons.getReference (0).states.size(), 2);
        expectEquals (tab.buttons.getReference (0).states.getReference (0).mainImage,
                      juce::String ("img:button_blue-off"));
        expectEquals (tab.images.size(), 1);

        expect (r.assets.containsKey ("img:Knob"));
        expectEquals (r.assets ["img:Knob"], juce::String ("Resources/Knob.png"));
        expect (r.assets.containsKey ("img:background_bass"));
        expect (r.assets.containsKey ("img:button_blue-off"));
        expect (r.assets.containsKey ("img:light_off"));
    }

    void testMaskinTrommer()
    {
        beginTest ("MaskinTrommer.dspreset → round-robin / velocity layers / gain");
        auto r = dmconv::parseDspreset (load ("maskintrommer", "MaskinTrommer.dspreset"), "Kit");
        expect (r.ok, "MaskinTrommer should parse: " + r.errors.joinIntoString ("; "));

        bool hasRoundRobin = false, hasVelocityLayer = false, hasSeqPosition = false;
        for (const auto& g : r.mode.groups)
        {
            if (g.roundRobin && g.roundRobin->mode == "random") hasRoundRobin = true;
            if (g.velocity.has_value()) hasVelocityLayer = true;
            for (const auto& s : g.samples)
                if (s.seqPosition.has_value()) hasSeqPosition = true;
        }
        expect (hasRoundRobin, "expected a seqMode=random group");
        expect (hasVelocityLayer, "expected loVel/hiVel velocity layers");
        expect (hasSeqPosition, "expected samples with seqPosition");

        bool hasGain = false;
        for (const auto& fx : r.mode.effects)
            if (fx.type == "gain" && fx.gain.has_value())
                hasGain = true;
        expect (hasGain, "expected a gain effect with a level");
    }

    void testReleaseTriggerAndLfo()
    {
        beginTest ("Midnight Wurli.dspreset → release trigger + LFO modulator");
        auto r = dmconv::parseDspreset (load ("midnight-wurli", "Midnight Wurli.dspreset"), "Wurli");
        expect (r.ok, "Midnight Wurli should parse: " + r.errors.joinIntoString ("; "));

        bool hasReleaseTrigger = false;
        for (const auto& g : r.mode.groups)
            if (g.trigger == "release")
                hasReleaseTrigger = true;
        expect (hasReleaseTrigger, "expected a release-trigger group");

        expect (! r.mode.modulators.isEmpty(), "expected an LFO modulator");
        if (! r.mode.modulators.isEmpty())
            expect (! r.mode.modulators.getReference (0).bindings.isEmpty(),
                    "expected the LFO to carry bindings");
    }

    void testRoundTrip()
    {
        beginTest ("parsed Bass → manifest → reload");
        auto r = dmconv::parseDspreset (load ("omni-84", "Bass.dspreset"), "Bass");
        expect (r.ok);

        dm::PresetLibrary lib;
        lib.schema = dm::kManifestSchemaVersion;
        lib.format = "dmse-manifest";
        lib.library = "Omni-84";
        lib.modes.add (r.mode);

        const auto json = dm::writeManifestToJson (lib);
        auto reloaded = dm::loadManifestFromJson (json);
        expect (reloaded.ok, "manifest from parsed preset should reload: "
                              + reloaded.errors.joinIntoString ("; "));
        expectEquals (reloaded.library.modes.size(), 1);
        expectEquals (reloaded.library.modes.getReference (0).groups.getReference (0)
                          .samples.getReference (0).source,
                      juce::String ("flac:Bass_0C"));
    }
};

DspresetParserTests dspresetParserTests;

class PrintingRunner : public juce::UnitTestRunner
{
    void logMessage (const juce::String& message) override { std::cout << message << std::endl; }
};
} // namespace

int main()
{
    PrintingRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0, total = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* res = runner.getResult (i);
        failures += res->failures;
        total    += res->passes + res->failures;
    }
    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " — " << total << " checks, " << failures << " failure(s)" << std::endl;
    return failures == 0 ? 0 : 1;
}
