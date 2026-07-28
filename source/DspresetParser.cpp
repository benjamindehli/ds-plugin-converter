#include "DspresetParser.h"
#include <map>
#include <set>
#include <utility>   // std::swap

namespace dmconv
{
// A control's resolved engine target + range, keyed by its document-order index,
// so a <midi><cc> binding's controlIndex can be mapped to a parameter.
struct UiControlTarget
{
    juce::String parameter;
    std::optional<int> groupIndex;
    double min = 0.0, max = 1.0;
};
}

namespace dmconv
{

namespace
{
using juce::XmlElement;

juce::StringArray splitTags (const juce::String& s)
{
    juce::StringArray t;
    t.addTokens (s, " ,", "");
    t.removeEmptyStrings();
    t.trim();
    return t;
}

bool toBool (const juce::String& s)
{
    return s == "1" || s.equalsIgnoreCase ("true") || s.equalsIgnoreCase ("yes");
}

// File-name stem of a (possibly relative, possibly Windows-slashed) path.
juce::String stem (const juce::String& path)
{
    auto p = path.replaceCharacter ('\\', '/');
    auto base = p.fromLastOccurrenceOf ("/", false, false);
    return base.upToLastOccurrenceOf (".", false, false);
}

std::optional<double> optD (const XmlElement& e, juce::StringRef n)
{
    return e.hasAttribute (n) ? std::optional<double> (e.getDoubleAttribute (n)) : std::nullopt;
}
std::optional<int> optI (const XmlElement& e, juce::StringRef n)
{
    return e.hasAttribute (n) ? std::optional<int> (e.getIntAttribute (n)) : std::nullopt;
}
std::optional<bool> optB (const XmlElement& e, juce::StringRef n)
{
    return e.hasAttribute (n) ? std::optional<bool> (toBool (e.getStringAttribute (n))) : std::nullopt;
}

// Register an asset id → source path. Asset ids are keyed on the file-name STEM, so
// two DIFFERENT files sharing a basename (e.g. Samples/A/Hit.wav and Samples/B/Hit.wav)
// would silently collapse into one id — every reference to the first then plays/shows
// the wrong asset. That's guaranteed corruption, so a conflicting re-registration is
// an ERROR (the first path is kept).
void registerAsset (ParseResult& res, const juce::String& id, const juce::String& path)
{
    auto normalise = [] (const juce::String& p)
    {
        auto n = p.replaceCharacter ('\\', '/');
        while (n.startsWith ("./"))
            n = n.substring (2);
        return n;
    };
    const auto existing = res.assets.getValue (id, {});
    if (existing.isNotEmpty() && normalise (existing) != normalise (path))
    {
        res.errors.add ("asset id collision: '" + id + "' refers to both \"" + existing
                        + "\" and \"" + path + "\" — rename one file (ids key on the basename)");
        return;
    }
    res.assets.set (id, path);
}

// Register an image asset (bg, knob skin, button state, light, PATH swap). Stored
// in the asset table with an "img:" id; the converter copies these verbatim
// (no transcode). Returns the asset id, or "" for an empty path.
juce::String registerImage (ParseResult& res, const juce::String& path)
{
    if (path.isEmpty())
        return {};
    const auto id = "img:" + stem (path);
    registerAsset (res, id, path);
    return id;
}

juce::var parseTranslationValue (ParseResult& res, const juce::String& s)
{
    if (s.equalsIgnoreCase ("true"))  return juce::var (true);
    if (s.equalsIgnoreCase ("false")) return juce::var (false);
    if (s.endsWithIgnoreCase (".png") || s.endsWithIgnoreCase (".jpg") || s.endsWithIgnoreCase (".jpeg"))
        return juce::var (registerImage (res, s));   // PATH image swap → asset id
    if (s.endsWithIgnoreCase (".wav") || s.endsWithIgnoreCase (".flac"))
    {
        // FX_IR_FILE swap (menu option or button state) → register + use the asset id.
        const auto id = "ir:" + stem (s);
        registerAsset (res, id, s);
        return juce::var (id);
    }
    return juce::var (s);
}

dm::Binding parseBinding (const XmlElement& e, ParseResult& res)
{
    dm::Binding b;
    b.type        = e.getStringAttribute ("type");
    b.level       = e.getStringAttribute ("level");
    b.parameter   = e.getStringAttribute ("parameter");
    b.translation = e.getStringAttribute ("translation");
    b.modBehavior = e.getStringAttribute ("modBehavior");
    b.identifier  = e.getStringAttribute ("identifier");        // tag name (level="tag")
    b.translationTable = e.getStringAttribute ("translationTable");
    b.translationReversed = e.getBoolAttribute ("translationReversed", false);

    b.factor               = optD (e, "factor");
    b.modAmount            = optD (e, "modAmount");
    b.translationOutputMin = optD (e, "translationOutputMin");
    b.translationOutputMax = optD (e, "translationOutputMax");

    b.effectIndex  = optI (e, "effectIndex");
    b.controlIndex = optI (e, "controlIndex");
    b.groupIndex   = optI (e, "groupIndex");
    b.noteIndex    = optI (e, "noteIndex");
    b.bindingIndex = optI (e, "bindingIndex");
    b.seqIndex     = optI (e, "seqIndex");
    b.position     = optI (e, "position");
    // DecentSampler references a specific modulator (LFO) with `modulatorIndex` on
    // control/button bindings; carry it as the binding's position so the engine can
    // address that modulator (e.g. a hi-fi/lo-fi switch setting each modulator's depth).
    if (! b.position)
        b.position = optI (e, "modulatorIndex");

    if (e.hasAttribute ("translationValue"))
        b.translationValue = parseTranslationValue (res, e.getStringAttribute ("translationValue"));

    // DecentSampler `position` is the 0-based index within the binding's level —
    // normalise it to the explicit index the engine resolves against, so the rest
    // of the pipeline only deals with groupIndex/effectIndex/controlIndex. (A
    // modulatorIndex-sourced position on a type="modulator" binding is left in
    // `position` — none of these levels match — for the engine's LFO routing.)
    if (b.position)
    {
        if (b.level == "group" && ! b.groupIndex)        b.groupIndex   = b.position;
        else if (b.type == "effect" && ! b.effectIndex)  b.effectIndex  = b.position;
        else if (b.level == "ui" && ! b.controlIndex)    b.controlIndex = b.position;
    }

    return b;
}

void parseAmp (const XmlElement& groups, dm::AmpEnvelope& amp)
{
    amp.attack   = groups.getDoubleAttribute ("attack", amp.attack);
    amp.decay    = groups.getDoubleAttribute ("decay", amp.decay);
    amp.sustain  = groups.getDoubleAttribute ("sustain", amp.sustain);
    amp.release  = groups.getDoubleAttribute ("release", amp.release);
    amp.volume   = groups.getDoubleAttribute ("volume", amp.volume);
    amp.velTrack = groups.getDoubleAttribute ("ampVelTrack", amp.velTrack);
    if (groups.hasAttribute ("ampEnvEnabled"))
        amp.enabled = toBool (groups.getStringAttribute ("ampEnvEnabled"));
    amp.attackCurve  = optD (groups, "attackCurve");
    amp.decayCurve   = optD (groups, "decayCurve");
    amp.releaseCurve = optD (groups, "releaseCurve");
}

dm::Sample parseSample (const XmlElement& e, ParseResult& res)
{
    dm::Sample s;
    const auto path = e.getStringAttribute ("path");
    if (path.isEmpty())
        res.errors.add ("sample missing path");

    const auto id = "flac:" + stem (path);
    s.source = id;
    if (path.isNotEmpty())
        registerAsset (res, id, path);

    s.loNote   = e.getIntAttribute ("loNote", 0);
    s.hiNote   = e.getIntAttribute ("hiNote", 127);
    s.rootNote = e.getIntAttribute ("rootNote", 60);

    s.lengthFrames  = optI (e, "length");
    s.sampleRate    = optD (e, "sampleRate");
    s.pitchKeyTrack = toBool (e.getStringAttribute ("pitchKeyTrack", "0"));
    // A FRACTIONAL pitchKeyTrack (0<x<1) is used by string-machine libraries as a
    // per-sample pitch-DRIFT depth (bass drifts less than treble), not key-tracking.
    // (Samples are one-per-note, so real key-tracking is moot here.)
    if (const double pkt = e.getStringAttribute ("pitchKeyTrack").getDoubleValue();
        pkt > 0.0 && pkt < 1.0)
        s.pitchDrift = pkt;

    s.start       = optI (e, "start");
    s.end         = optI (e, "end");
    s.volume      = optD (e, "volume");
    s.seqPosition = optI (e, "seqPosition");
    s.ampEnvEnabled = optB (e, "ampEnvEnabled");
    s.onLoCC64    = optI (e, "onLoCC64");
    s.onHiCC64    = optI (e, "onHiCC64");

    if (toBool (e.getStringAttribute ("loopEnabled", "0")))
    {
        s.loop.enabled   = true;
        s.loop.start     = optI (e, "loopStart");
        s.loop.end       = optI (e, "loopEnd");
        s.loop.crossfade = optI (e, "loopCrossfade");
    }
    return s;
}

dm::Effect parseEffect (const XmlElement& e, ParseResult& res);   // defined below

dm::Group parseGroup (const XmlElement& e, ParseResult& res)
{
    dm::Group g;
    g.uid               = e.getStringAttribute ("uid");
    g.tags              = splitTags (e.getStringAttribute ("tags"));
    g.trigger           = e.getStringAttribute ("trigger");
    g.loopCrossfadeMode = e.getStringAttribute ("loopCrossfadeMode");

    if (e.hasAttribute ("loVel") || e.hasAttribute ("hiVel"))
    {
        dm::VelocityRange v;
        v.lo = e.getIntAttribute ("loVel", 0);
        v.hi = e.getIntAttribute ("hiVel", 127);
        g.velocity = v;
    }
    if (e.hasAttribute ("seqMode"))
    {
        dm::RoundRobin rr;
        rr.mode   = e.getStringAttribute ("seqMode");
        rr.length = optI (e, "seqLength");
        g.roundRobin = rr;
    }
    if (e.hasAttribute ("silencingMode") || e.hasAttribute ("silencedByTags"))
    {
        dm::Silencing s;
        s.mode   = e.getStringAttribute ("silencingMode");
        s.byTags = splitTags (e.getStringAttribute ("silencedByTags"));
        g.silencing = s;
    }

    g.attack        = optD (e, "attack");
    g.decay         = optD (e, "decay");
    g.sustain       = optD (e, "sustain");
    g.release       = optD (e, "release");
    g.volume        = optD (e, "volume");
    g.velTrack      = optD (e, "ampVelTrack");
    g.ampEnvEnabled = optB (e, "ampEnvEnabled");
    g.pitchKeyTrack = optB (e, "pitchKeyTrack");

    // Per-group insert effects (e.g. an organ stop's swell lowpass + loudness gain).
    if (auto* fxList = e.getChildByName ("effects"))
        for (auto* fx : fxList->getChildIterator())
            if (fx->hasTagName ("effect"))
                g.effects.add (parseEffect (*fx, res));

    for (auto* child : e.getChildIterator())
        if (child->hasTagName ("sample"))
            g.samples.add (parseSample (*child, res));

    return g;
}

dm::Effect parseEffect (const XmlElement& e, ParseResult& res)
{
    dm::Effect fx;
    fx.type = e.getStringAttribute ("type");
    if (e.hasAttribute ("enabled"))
        fx.enabled = toBool (e.getStringAttribute ("enabled"));

    fx.frequency   = optD (e, "frequency");
    fx.resonance   = optD (e, "resonance");
    fx.drive       = optD (e, "drive");
    fx.mix         = optD (e, "mix");
    fx.wet         = optD (e, "wetLevel");
    fx.outputLevel = optD (e, "outputLevel");
    fx.rate        = optD (e, "modRate");    // chorus
    fx.depth       = optD (e, "modDepth");   // chorus
    fx.feedback    = optD (e, "feedback");   // chorus

    // `gain` effect carries its amount in "level".
    if (fx.type == "gain")
        fx.gain = optD (e, "level");

    if (e.hasAttribute ("irFile"))
    {
        const auto ir = e.getStringAttribute ("irFile");
        const auto id = "ir:" + stem (ir);
        fx.ir = id;
        registerAsset (res, id, ir);
    }
    return fx;
}

dm::NoteSequence parseSequence (const XmlElement& e)
{
    dm::NoteSequence seq;
    seq.name   = e.getStringAttribute ("name");
    seq.length = optI (e, "length");
    seq.rate   = optD (e, "rate");

    for (auto* n : e.getChildIterator())
        if (n->hasTagName ("note"))
        {
            dm::SequenceNote note;
            note.position     = n->getIntAttribute ("position", 0);
            note.note         = n->getIntAttribute ("note", 60);
            note.velocity     = n->getDoubleAttribute ("velocity", 1.0);
            note.length       = n->getDoubleAttribute ("length", 1.0);
            if (n->hasAttribute ("enabled"))
                note.enabled = toBool (n->getStringAttribute ("enabled"));
            note.swallowNotes = toBool (n->getStringAttribute ("swallowNotes", "0"));
            seq.notes.add (note);
        }
    return seq;
}

dm::Lfo parseLfo (const XmlElement& e, ParseResult& res)
{
    dm::Lfo l;
    l.shape     = e.getStringAttribute ("shape");
    l.frequency = e.getDoubleAttribute ("frequency", 0.0);
    l.modAmount = e.getDoubleAttribute ("modAmount", 0.0);
    for (auto* b : e.getChildIterator())
        if (b->hasTagName ("binding"))
            l.bindings.add (parseBinding (*b, res));
    return l;
}

dm::Rect parseRect (const XmlElement& e)
{
    return { e.getIntAttribute ("x"), e.getIntAttribute ("y"),
             e.getIntAttribute ("width"), e.getIntAttribute ("height") };
}

dm::Control parseControl (const XmlElement& e, ParseResult& res)
{
    dm::Control c;
    c.rect      = parseRect (e);
    c.label     = e.getStringAttribute ("parameterName");
    c.valueType = e.getStringAttribute ("type");
    c.stepped   = e.getStringAttribute ("valueType").equalsIgnoreCase ("integer");   // DS integer knob → snap to steps
    c.min       = optD (e, "minValue");
    c.max       = optD (e, "maxValue");
    c.value     = optD (e, "value");
    c.textColor = e.getStringAttribute ("textColor");
    c.style     = e.getStringAttribute ("style");
    c.mouseDragSensitivity = optD (e, "mouseDragSensitivity");
    c.visible   = e.getBoolAttribute ("visible", true);

    if (e.hasAttribute ("customSkinImage"))
    {
        dm::CustomSkin skin;
        skin.image       = registerImage (res, e.getStringAttribute ("customSkinImage"));
        skin.numFrames   = optI (e, "customSkinNumFrames");
        skin.orientation = e.getStringAttribute ("customSkinImageOrientation");
        c.skin = skin;
    }

    for (auto* b : e.getChildIterator())
        if (b->hasTagName ("binding"))
            c.bindings.add (parseBinding (*b, res));
    return c;
}

dm::Button parseButton (const XmlElement& e, ParseResult& res)
{
    dm::Button b;
    b.rect  = parseRect (e);
    b.style = e.getStringAttribute ("style");
    b.value = optI (e, "value");
    b.visible = e.getBoolAttribute ("visible", true);

    for (auto* s : e.getChildIterator())
        if (s->hasTagName ("state"))
        {
            dm::ButtonState st;
            st.name       = s->getStringAttribute ("name");
            st.mainImage  = registerImage (res, s->getStringAttribute ("mainImage"));
            st.hoverImage = registerImage (res, s->getStringAttribute ("hoverImage"));
            st.clickImage = registerImage (res, s->getStringAttribute ("clickImage"));
            for (auto* bnd : s->getChildIterator())
                if (bnd->hasTagName ("binding"))
                    st.bindings.add (parseBinding (*bnd, res));
            b.states.add (st);
        }
    return b;
}

dm::UiImage parseImage (const XmlElement& e, ParseResult& res)
{
    dm::UiImage img;
    img.rect            = parseRect (e);
    img.image           = registerImage (res, e.getStringAttribute ("path"));
    img.aspectRatioMode = e.getStringAttribute ("aspectRatioMode");
    img.visible         = e.getBoolAttribute ("visible", true);
    return img;
}

dm::Menu parseMenu (const XmlElement& e, ParseResult& res)
{
    dm::Menu m;
    m.rect  = parseRect (e);
    m.value = e.getIntAttribute ("value", 1);
    m.visible = e.getBoolAttribute ("visible", true);
    m.textColor       = e.getStringAttribute ("textColor");
    m.backgroundColor = e.getStringAttribute ("backgroundColor");
    m.hAlign          = e.getStringAttribute ("hAlign");

    for (auto* o : e.getChildIterator())
        if (o->hasTagName ("option"))
        {
            dm::MenuOption mo;
            mo.name = o->getStringAttribute ("name");
            for (auto* b : o->getChildIterator())
            {
                if (! b->hasTagName ("binding"))
                    continue;

                // The SEQ_INDEX this option selects = its noteIndex-0 binding value
                // (Omni-84's options offset the whole sequence block by 0/84/168/252).
                if (b->getStringAttribute ("parameter") == "SEQ_INDEX"
                    && b->getIntAttribute ("noteIndex", -1) == 0)
                    mo.seqIndex = b->getIntAttribute ("translationValue", 0);

                // Effect bindings (amp/cabinet selector etc.) are applied on selection.
                // FX_IR_FILE paths are registered + rewritten to asset ids inside
                // parseBinding (via parseTranslationValue), same as button states.
                mo.bindings.add (parseBinding (*b, res));
            }
            m.options.add (mo);
        }
    return m;
}

// Parse a container's UI children into a Tab, assigning each element its
// document-order index (DecentSampler's controlIndex). PATH bindings address
// lights by that index, so it must count every control/button/image/menu in order.
void parseUiChildren (const XmlElement& parent, dm::Tab& tab, ParseResult& res,
                      std::map<int, UiControlTarget>& controlsByIndex,
                      std::set<int>& menuIndices)
{
    int uiIndex = 0;
    std::map<juce::String, int> labelSeen;   // de-duplicate control labels within this tab
    for (auto* node : parent.getChildIterator())
    {
        if (node->hasTagName ("control"))
        {
            auto c = parseControl (*node, res);
            c.controlIndex = uiIndex;   // document-order index (VISIBLE/OPACITY binding target)

            // Per-control params are keyed by label, so two controls sharing a label
            // in the same mode would collapse to ONE param and move together (e.g.
            // EDB-Orgel has a hidden vibrato helper mislabelled the same as a visible
            // velocity knob). Give the 2nd+ occurrence a distinct label → its own param.
            if (c.label.isNotEmpty())
            {
                const int n = ++labelSeen[c.label];
                if (n > 1)
                    c.label = c.label + " (" + juce::String (n) + ")";
            }

            if (! c.bindings.isEmpty())   // record target so <cc>/<note> controlIndex can resolve it
            {
                UiControlTarget t;
                t.parameter  = c.bindings.getReference (0).parameter;
                t.groupIndex = c.bindings.getReference (0).groupIndex;
                t.min = c.min.value_or (0.0);
                t.max = c.max.value_or (1.0);
                controlsByIndex[uiIndex] = t;
            }
            tab.controls.add (c);
            ++uiIndex;
        }
        else if (node->hasTagName ("button"))  { auto b = parseButton (*node, res); b.controlIndex = uiIndex; tab.buttons.add (b); ++uiIndex; }
        else if (node->hasTagName ("image"))   { auto im = parseImage (*node, res); im.controlIndex = uiIndex++; tab.images.add (im); }
        else if (node->hasTagName ("menu"))    { auto m = parseMenu (*node, res); m.controlIndex = uiIndex; menuIndices.insert (uiIndex); tab.menus.add (m); ++uiIndex; }
    }
}

void parseUi (const XmlElement& e, dm::Ui& ui, ParseResult& res,
              std::map<int, UiControlTarget>& controlsByIndex,
              std::set<int>& menuIndices)
{
    ui.background = registerImage (res, e.getStringAttribute ("bgImage"));
    ui.width      = e.getIntAttribute ("width", 0);
    ui.height     = e.getIntAttribute ("height", 0);
    ui.layoutMode = e.getStringAttribute ("layoutMode");
    ui.bgMode     = e.getStringAttribute ("bgMode");

    dm::Tab loose;   // controls placed directly under <ui> (no <tab>)
    bool sawTab = false;

    for (auto* ch : e.getChildIterator())
    {
        if (ch->hasTagName ("tab"))
        {
            dm::Tab tab;
            tab.name = ch->getStringAttribute ("name");
            parseUiChildren (*ch, tab, res, controlsByIndex, menuIndices);
            ui.tabs.add (tab);
            sawTab = true;
        }
    }

    // Controls placed directly under <ui> (no <tab>) — own document-order index.
    parseUiChildren (e, loose, res, controlsByIndex, menuIndices);

    if (! loose.controls.isEmpty() || ! loose.buttons.isEmpty()
        || ! loose.images.isEmpty() || ! loose.menus.isEmpty())
    {
        if (sawTab)
        {
            auto& t = ui.tabs.getReference (0);
            t.controls.addArray (loose.controls);
            t.buttons.addArray (loose.buttons);
            t.images.addArray (loose.images);
            t.menus.addArray (loose.menus);
        }
        else
        {
            ui.tabs.add (loose);
        }
    }

    if (auto* kb = e.getChildByName ("keyboard"))
        for (auto* c : kb->getChildIterator())
            if (c->hasTagName ("color"))
            {
                dm::KeyboardColor kc;
                kc.loNote = c->getIntAttribute ("loNote", 0);
                kc.hiNote = c->getIntAttribute ("hiNote", 127);
                kc.color  = c->getStringAttribute ("color");
                ui.keyboardColors.add (kc);
            }
}
} // namespace

ParseResult parseDspreset (const juce::String& xmlText, const juce::String& modeName)
{
    ParseResult res;

    auto xml = juce::XmlDocument::parse (xmlText);
    if (xml == nullptr)
    {
        res.errors.add ("invalid XML");
        return res;
    }
    if (! xml->hasTagName ("DecentSampler"))
        res.errors.add ("root element is not <DecentSampler>");

    res.mode.name = modeName;

    if (auto* groups = xml->getChildByName ("groups"))
    {
        parseAmp (*groups, res.mode.amp);
        for (auto* g : groups->getChildIterator())
            if (g->hasTagName ("group"))
                res.mode.groups.add (parseGroup (*g, res));
    }
    else
    {
        res.errors.add ("missing <groups>");
    }

    if (auto* effects = xml->getChildByName ("effects"))
        for (auto* fx : effects->getChildIterator())
            if (fx->hasTagName ("effect"))
                res.mode.effects.add (parseEffect (*fx, res));

    if (auto* seqs = xml->getChildByName ("noteSequences"))
        for (auto* s : seqs->getChildIterator())
            if (s->hasTagName ("sequence"))
                res.mode.sequences.add (parseSequence (*s));

    // <midi> note → note_sequence bindings become the engine's sequenceTriggers
    // (each key fires a sequence). The sequences are absolute, so transpose = 0;
    // the chord-ordering menu's SEQ_INDEX offset is applied at runtime.
    if (auto* midi = xml->getChildByName ("midi"))
        for (auto* n : midi->getChildIterator())
            if (n->hasTagName ("note"))
                for (auto* b : n->getChildIterator())
                    if (b->hasTagName ("binding")
                        && b->getStringAttribute ("type") == "note_sequence")
                    {
                        dm::SequenceTrigger t;
                        t.note          = n->getIntAttribute ("note", 60);
                        t.sequence      = b->getIntAttribute ("seqIndex", 0);
                        t.transpose     = 0;
                        t.rate          = b->getDoubleAttribute ("seqPlaybackRate", 10.0);
                        t.loop          = b->getStringAttribute ("seqLoopMode") == "loop";
                        t.trackVelocity = toBool (b->getStringAttribute ("seqTrackMidiInputVelocity", "1"));
                        t.swallow       = toBool (n->getStringAttribute ("swallowNotes", "0"));
                        res.mode.sequenceTriggers.add (t);
                    }

    if (auto* mods = xml->getChildByName ("modulators"))
        for (auto* m : mods->getChildIterator())
            if (m->hasTagName ("lfo"))
                res.mode.modulators.add (parseLfo (*m, res));

    if (auto* tags = xml->getChildByName ("tags"))
        for (auto* t : tags->getChildIterator())
            if (t->hasTagName ("tag"))
            {
                dm::Tag tag;
                tag.name      = t->getStringAttribute ("name");
                tag.polyphony = optI (*t, "polyphony");
                res.mode.tags.add (tag);
            }

    std::map<int, UiControlTarget> controlsByIndex;
    std::set<int> menuIndices;
    if (auto* ui = xml->getChildByName ("ui"))
        parseUi (*ui, res.mode.ui, res, controlsByIndex, menuIndices);

    // <midi> bindings that target the UI (must be parsed AFTER <ui>):
    //   <cc number="N">  → CC controls a parameter (e.g. mod wheel → StrumSpeed)
    //   <note ...>       → a key selects a chord-order menu option (key-switch)
    if (auto* midi = xml->getChildByName ("midi"))
        for (auto* el : midi->getChildIterator())
        {
            if (el->hasTagName ("cc"))
            {
                const int ccNum = el->getIntAttribute ("number", 1);
                for (auto* b : el->getChildIterator())
                    if (b->hasTagName ("binding"))
                    {
                        // The target control is addressed by controlIndex OR position
                        // (DecentSampler uses either; both are the doc-order UI index).
                        auto ci = optI (*b, "controlIndex");
                        if (! ci) ci = optI (*b, "position");
                        if (! ci)
                            continue;
                        const auto found = controlsByIndex.find (*ci);
                        if (found == controlsByIndex.end())
                        {
                            res.warnings.add ("CC " + juce::String (ccNum) + " binding -> unknown controlIndex "
                                              + juce::String (*ci));
                            continue;
                        }
                        const auto& tgt = found->second;
                        const double span   = tgt.max - tgt.min;
                        const double outMin = b->getDoubleAttribute ("translationOutputMin", tgt.min);
                        const double outMax = b->getDoubleAttribute ("translationOutputMax", tgt.max);
                        dm::CcBinding cb;
                        cb.cc           = ccNum;
                        cb.parameter    = tgt.parameter;
                        cb.groupIndex   = tgt.groupIndex;
                        cb.controlIndex = *ci;   // the specific target → drive only this control
                        const bool hasSpan = ! juce::exactlyEqual (span, 0.0);
                        cb.normMin    = hasSpan ? (outMin - tgt.min) / span : 0.0;
                        cb.normMax    = hasSpan ? (outMax - tgt.min) / span : 1.0;
                        if (b->getBoolAttribute ("translationReversed", false))
                            std::swap (cb.normMin, cb.normMax);   // mod wheel inverted (e.g. Swell)
                        res.mode.ccBindings.add (cb);
                    }
            }
            else if (el->hasTagName ("note"))
            {
                const int note = el->getIntAttribute ("note", -1);
                for (auto* b : el->getChildIterator())
                    if (b->hasTagName ("binding")
                        && b->getStringAttribute ("type") == "control"
                        && b->getStringAttribute ("parameter") == "VALUE")
                    {
                        const auto ci = optI (*b, "controlIndex");
                        if (ci && menuIndices.count (*ci))   // key-switch onto a (chord-order) menu
                        {
                            dm::MenuKeySwitch ks;
                            ks.note   = note;
                            ks.option = b->getIntAttribute ("translationValue", 1) - 1;   // 1-based → 0-based
                            res.mode.menuKeySwitches.add (ks);
                        }
                        // note→knob VALUE switches: not needed yet (surface when a library uses them).
                    }
            }
        }

    // --- ID-based bindings (Phase 1: instrument effects) --------------------
    // Assign each instrument effect a stable, readable id ("fx_<type>[_n]"), then
    // rewrite every UI binding that targets one by index into an id reference
    // (targetId). The engine resolves targetId → slot at load; the effectIndex is
    // still emitted as a transition fallback.
    {
        std::map<juce::String, int> typeCount;
        for (auto& e : res.mode.effects)
        {
            const int n = ++typeCount[e.type];
            e.id = "fx_" + (e.type.isNotEmpty() ? e.type : juce::String ("effect"))
                 + (n > 1 ? "_" + juce::String (n) : juce::String());
        }

        // Phase 2: groups. Every group gets a stable uid (kept if the preset already
        // set one, else generated). A group binding — group-level params AND group-effect
        // bindings (which keep effectIndex as the slot within that group's chain) — is
        // rewritten to reference the group by uid.
        int gn = 0;
        for (auto& g : res.mode.groups)
        {
            if (g.uid.isEmpty())
                g.uid = "grp_" + juce::String (gn);
            ++gn;
        }

        // Group effects: each per-group insert effect gets a stable id scoped by its
        // group's uid, so a group-effect binding can target it by that single id instead
        // of a (groupIndex, effectIndex) pair (e.g. an organ's per-stop swell filter).
        for (auto& g : res.mode.groups)
        {
            std::map<juce::String, int> gTypeCount;
            for (auto& e : g.effects)
            {
                const int n = ++gTypeCount[e.type];
                e.id = g.uid + "_fx_" + (e.type.isNotEmpty() ? e.type : juce::String ("effect"))
                     + (n > 1 ? "_" + juce::String (n) : juce::String());
            }
        }

        // Phase 3: modulators (LFOs). Each gets a stable id; MOD_AMOUNT/FREQUENCY
        // bindings (which target a modulator via position = DecentSampler modulatorIndex)
        // are rewritten to reference it by id.
        int modCount = 0;
        for (auto& m : res.mode.modulators)
            m.id = "mod_" + juce::String (modCount++);

        auto resolveEffectTarget = [&res] (dm::Binding& b)
        {
            if (b.targetId.isNotEmpty() || b.level == "group")   // group effects → resolveGroupTarget
                return;
            if (b.effectIndex && *b.effectIndex >= 0 && *b.effectIndex < res.mode.effects.size())
                b.targetId = res.mode.effects.getReference (*b.effectIndex).id;
        };
        auto resolveGroupEffectTarget = [&res] (dm::Binding& b)
        {
            if (b.targetId.isNotEmpty())
                return;
            // A group-effect binding carries both a groupIndex (which group) and an
            // effectIndex (which slot in that group's chain) — rewrite it to the
            // effect's own id so the slot is addressed by id, not position.
            if (b.level == "group" && b.effectIndex && b.groupIndex
                && *b.groupIndex >= 0 && *b.groupIndex < res.mode.groups.size())
            {
                auto& g = res.mode.groups.getReference (*b.groupIndex);
                if (*b.effectIndex >= 0 && *b.effectIndex < g.effects.size())
                    b.targetId = g.effects.getReference (*b.effectIndex).id;
            }
        };
        auto resolveGroupTarget = [&res] (dm::Binding& b)
        {
            if (b.targetId.isNotEmpty())   // already an effect/group-effect target or resolved
                return;
            if (b.groupIndex && *b.groupIndex >= 0 && *b.groupIndex < res.mode.groups.size())
                b.targetId = res.mode.groups.getReference (*b.groupIndex).uid;
        };
        auto resolveModulatorTarget = [&res] (dm::Binding& b)
        {
            if (b.targetId.isNotEmpty())
                return;
            if ((b.parameter == "MOD_AMOUNT" || b.parameter == "FREQUENCY")
                && b.position && *b.position >= 0 && *b.position < res.mode.modulators.size())
                b.targetId = res.mode.modulators.getReference (*b.position).id;
        };
        auto resolve = [&] (dm::Binding& b)
        {
            resolveEffectTarget (b);
            resolveGroupEffectTarget (b);
            resolveGroupTarget (b);
            resolveModulatorTarget (b);
        };

        for (auto& tab : res.mode.ui.tabs)
        {
            for (auto& c : tab.controls) for (auto& b : c.bindings) resolve (b);
            for (auto& bt : tab.buttons)  for (auto& st : bt.states)  for (auto& b : st.bindings) resolve (b);
            for (auto& mn : tab.menus)    for (auto& op : mn.options)  for (auto& b : op.bindings) resolve (b);
        }
        // Modulator bindings drive groups (tremolo/tuning by uid) AND per-group effects
        // (e.g. Elektrisk's tremulant swelling a group's gain slot) — run the full
        // resolve so group-effect targets become effect ids, not just the group uid.
        for (auto& lfo : res.mode.modulators)
            for (auto& b : lfo.bindings)
                resolve (b);

        // Phase 4: UI elements. Each control/button/menu/image gets a stable id; bindings
        // that target one by controlIndex — PATH (light image swap), VISIBLE/OPACITY, and
        // VALUE (button/menu cross-refs) — plus the mode's <cc> bindings are rewritten to
        // reference the target by id. Ids are per-tab (controlIndex is per-tab doc order).
        for (auto& tab : res.mode.ui.tabs)
        {
            std::map<int, juce::String> ciToId;
            int cn = 0, bn = 0, mnn = 0, imn = 0;
            for (auto& c  : tab.controls) { c.id  = "ctl_"  + juce::String (cn++);  if (c.controlIndex)  ciToId[*c.controlIndex]  = c.id; }
            for (auto& bt : tab.buttons)  { bt.id = "btn_"  + juce::String (bn++);  if (bt.controlIndex) ciToId[*bt.controlIndex] = bt.id; }
            for (auto& mn : tab.menus)    { mn.id = "menu_" + juce::String (mnn++); if (mn.controlIndex) ciToId[*mn.controlIndex] = mn.id; }
            for (auto& im : tab.images)   { im.id = "img_"  + juce::String (imn++); if (im.controlIndex) ciToId[*im.controlIndex] = im.id; }

            auto resolveUiTarget = [&ciToId] (dm::Binding& b)
            {
                if (b.targetId.isNotEmpty() || ! b.controlIndex)
                    return;
                if (b.parameter != "PATH" && b.parameter != "VISIBLE"
                    && b.parameter != "OPACITY" && b.parameter != "VALUE")
                    return;
                if (auto it = ciToId.find (*b.controlIndex); it != ciToId.end())
                    b.targetId = it->second;
            };
            for (auto& c  : tab.controls) for (auto& b : c.bindings) resolveUiTarget (b);
            for (auto& bt : tab.buttons)  for (auto& st : bt.states)  for (auto& b : st.bindings) resolveUiTarget (b);
            for (auto& mn : tab.menus)    for (auto& op : mn.options)  for (auto& b : op.bindings) resolveUiTarget (b);

            // <cc> bindings are mode-level but target controls in this tab.
            for (auto& cb : res.mode.ccBindings)
                if (cb.targetId.isEmpty() && cb.controlIndex)
                    if (auto it = ciToId.find (*cb.controlIndex); it != ciToId.end())
                        cb.targetId = it->second;
        }

        // Validation: bindings are id-based now, so the manifest writer drops the
        // positional element indices. Any binding that still carries one without a
        // resolved targetId would silently lose its target — fail the conversion loudly
        // here instead, so a resolve-pass gap is caught at build time, not at runtime.
        auto needsId = [] (const dm::Binding& b)
        {
            return b.targetId.isEmpty()
                && (b.effectIndex.has_value() || b.groupIndex.has_value()
                    || b.controlIndex.has_value() || b.position.has_value());
        };
        auto check = [&] (const dm::Binding& b, const juce::String& where)
        {
            if (needsId (b))
                res.errors.add ("binding target did not resolve to an id (" + where
                                + ", parameter=" + b.parameter + ")");
        };
        for (auto& tab : res.mode.ui.tabs)
        {
            for (auto& c  : tab.controls) for (auto& b : c.bindings) check (b, "control " + c.label);
            for (auto& bt : tab.buttons)  for (auto& st : bt.states)  for (auto& b : st.bindings) check (b, "button " + bt.id);
            for (auto& mn : tab.menus)    for (auto& op : mn.options)  for (auto& b : op.bindings) check (b, "menu " + mn.id);
        }
        for (auto& lfo : res.mode.modulators)
            for (auto& b : lfo.bindings) check (b, "modulator " + lfo.id);
        for (auto& cb : res.mode.ccBindings)
            if (cb.targetId.isEmpty() && cb.controlIndex.has_value())
                res.errors.add ("cc binding target did not resolve to an id (cc="
                                + juce::String (cb.cc) + ")");
    }

    // A groups-less preset renders no sound — say so explicitly. (Without this,
    // ok=false with an EMPTY errors list made the converter skip the preset silently
    // while still reporting the run as OK.)
    if (res.errors.isEmpty() && res.mode.groups.isEmpty())
        res.errors.add ("<groups> contains no <group> elements — preset would be silent");

    res.ok = res.errors.isEmpty();
    return res;
}

} // namespace dmconv
