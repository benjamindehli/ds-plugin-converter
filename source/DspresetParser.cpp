#include "DspresetParser.h"

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

juce::var translationValueVar (const juce::String& s)
{
    if (s.equalsIgnoreCase ("true"))  return juce::var (true);
    if (s.equalsIgnoreCase ("false")) return juce::var (false);
    // image PATH payloads → asset id; otherwise keep the raw string.
    if (s.containsChar ('/') || s.endsWithIgnoreCase (".png") || s.endsWithIgnoreCase (".jpg"))
        return juce::var ("img:" + stem (s));
    return juce::var (s);
}

dm::Binding parseBinding (const XmlElement& e)
{
    dm::Binding b;
    b.type        = e.getStringAttribute ("type");
    b.level       = e.getStringAttribute ("level");
    b.parameter   = e.getStringAttribute ("parameter");
    b.translation = e.getStringAttribute ("translation");
    b.modBehavior = e.getStringAttribute ("modBehavior");

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

    if (e.hasAttribute ("translationValue"))
        b.translationValue = translationValueVar (e.getStringAttribute ("translationValue"));

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
        res.assets.set (id, path);

    s.loNote   = e.getIntAttribute ("loNote", 0);
    s.hiNote   = e.getIntAttribute ("hiNote", 127);
    s.rootNote = e.getIntAttribute ("rootNote", 60);

    s.lengthFrames  = optI (e, "length");
    s.sampleRate    = optD (e, "sampleRate");
    s.pitchKeyTrack = toBool (e.getStringAttribute ("pitchKeyTrack", "0"));

    s.start       = optI (e, "start");
    s.end         = optI (e, "end");
    s.volume      = optD (e, "volume");
    s.seqPosition = optI (e, "seqPosition");
    s.ampEnvEnabled = optB (e, "ampEnvEnabled");

    if (toBool (e.getStringAttribute ("loopEnabled", "0")))
    {
        s.loop.enabled   = true;
        s.loop.start     = optI (e, "loopStart");
        s.loop.end       = optI (e, "loopEnd");
        s.loop.crossfade = optI (e, "loopCrossfade");
    }
    return s;
}

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

    g.decay         = optD (e, "decay");
    g.release       = optD (e, "release");
    g.volume        = optD (e, "volume");
    g.velTrack      = optD (e, "ampVelTrack");
    g.ampEnvEnabled = optB (e, "ampEnvEnabled");
    g.pitchKeyTrack = optB (e, "pitchKeyTrack");

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

    // `gain` effect carries its amount in "level".
    if (fx.type == "gain")
        fx.gain = optD (e, "level");

    if (e.hasAttribute ("irFile"))
    {
        const auto ir = e.getStringAttribute ("irFile");
        const auto id = "ir:" + stem (ir);
        fx.ir = id;
        res.assets.set (id, ir);
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

dm::Lfo parseLfo (const XmlElement& e)
{
    dm::Lfo l;
    l.shape     = e.getStringAttribute ("shape");
    l.frequency = e.getDoubleAttribute ("frequency", 0.0);
    l.modAmount = e.getDoubleAttribute ("modAmount", 0.0);
    for (auto* b : e.getChildIterator())
        if (b->hasTagName ("binding"))
            l.bindings.add (parseBinding (*b));
    return l;
}

// M2: UI background, size and keyboard colours only. Full control/button/binding
// mapping lands with the UI renderer (M4).
void parseUi (const XmlElement& e, dm::Ui& ui)
{
    if (e.hasAttribute ("bgImage"))
        ui.background = "img:" + stem (e.getStringAttribute ("bgImage"));
    ui.width      = e.getIntAttribute ("width", 0);
    ui.height     = e.getIntAttribute ("height", 0);
    ui.layoutMode = e.getStringAttribute ("layoutMode");
    ui.bgMode     = e.getStringAttribute ("bgMode");

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

    if (auto* mods = xml->getChildByName ("modulators"))
        for (auto* m : mods->getChildIterator())
            if (m->hasTagName ("lfo"))
                res.mode.modulators.add (parseLfo (*m));

    if (auto* tags = xml->getChildByName ("tags"))
        for (auto* t : tags->getChildIterator())
            if (t->hasTagName ("tag"))
            {
                dm::Tag tag;
                tag.name      = t->getStringAttribute ("name");
                tag.polyphony = optI (*t, "polyphony");
                res.mode.tags.add (tag);
            }

    if (auto* ui = xml->getChildByName ("ui"))
        parseUi (*ui, res.mode.ui);

    res.ok = res.errors.isEmpty() && ! res.mode.groups.isEmpty();
    return res;
}

} // namespace dmconv
