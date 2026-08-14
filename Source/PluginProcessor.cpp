#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace listenator
{

namespace
{
    using APF   = juce::AudioParameterFloat;
    using APB   = juce::AudioParameterBool;
    using APC   = juce::AudioParameterChoice;
    using Range = juce::NormalisableRange<float>;

    juce::String keyName (int i)
    {
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return names[i % 12];
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout ListenatorProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto v = juce::ParameterID { "", 1 };

    auto pv = [] (const char* id, int ver) { return juce::ParameterID { id, ver }; };

    layout.add (std::make_unique<APB> (pv (pid::listen, 1),        "LISTEN", false));
    layout.add (std::make_unique<APB> (pv (pid::cleanupBypass, 1), "Cleanup Bypass", false));
    layout.add (std::make_unique<APB> (pv (pid::effectsBypass, 1), "Effects Bypass", false));

    juce::StringArray keys;
    for (int i = 0; i < 12; ++i) keys.add (keyName (i));
    layout.add (std::make_unique<APC> (pv (pid::keyRoot, 1), "Key", keys, 0));
    layout.add (std::make_unique<APC> (pv (pid::keyScale, 1), "Scale",
        juce::StringArray { "Chromatic", "Major", "Minor", "Harmonic Minor",
                            "Pentatonic Major", "Pentatonic Minor" }, 0));

    auto trim = [&] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<APF> (pv (id, 1), name, Range (0.0f, 2.0f, 0.01f), def));
    };

    trim (pid::eqAmount,      "EQ Amount",      1.0f);
    trim (pid::compAmount,    "Comp Amount",    1.0f);
    trim (pid::deEssAmount,   "De-Ess Amount",  1.0f);
    trim (pid::cleanupAmount, "Cleanup Amount", 1.0f);
    trim (pid::tuneAmount,    "Tune Amount",    1.0f);
    trim (pid::doublerMix,    "Doubler",        1.0f);
    trim (pid::driveAmount,   "Drive",          1.0f);
    trim (pid::widthAmount,   "Width",          1.0f);
    trim (pid::delayMix,      "Delay",          1.0f);
    trim (pid::reverbMix,     "Reverb",         1.0f);
    trim (pid::duckAmount,    "Duck",           1.0f);

    layout.add (std::make_unique<APF> (pv (pid::characterMix, 1), "Character",
                                       Range (0.0f, 1.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<APF> (pv (pid::outputGain, 1), "Output",
                                       Range (-24.0f, 24.0f, 0.1f), 0.0f));

    auto bp = [&] (const char* id, const char* name)
    {
        layout.add (std::make_unique<APB> (pv (id, 1), name, false));
    };

    bp (pid::bpHighPass, "Bypass HPF");        bp (pid::bpDeNoise,  "Bypass De-Noise");
    bp (pid::bpDeVerb,   "Bypass De-Verb");    bp (pid::bpGate,     "Bypass Gate");
    bp (pid::bpSurgical, "Bypass Surgical");   bp (pid::bpResonance,"Bypass Resonance");
    bp (pid::bpDeEss,    "Bypass De-Ess");     bp (pid::bpComp,     "Bypass Comp");
    bp (pid::bpTone,     "Bypass Tone");       bp (pid::bpLimiter,  "Bypass Limiter");
    bp (pid::bpPitchRepair, "Bypass Pitch Repair");

    bp (pid::bpAutoTune,  "Bypass Tune");      bp (pid::bpDoubler,  "Bypass Doubler");
    bp (pid::bpSaturation,"Bypass Sat");       bp (pid::bpExciter,  "Bypass Exciter");
    bp (pid::bpCharacter, "Bypass Character"); bp (pid::bpWidth,    "Bypass Width");
    bp (pid::bpDelay,     "Bypass Delay");     bp (pid::bpReverb,   "Bypass Reverb");

    return layout;
}

//==============================================================================
ListenatorProcessor::ListenatorProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "LISTENATOR", createLayout())
{
}

ListenatorProcessor::~ListenatorProcessor() = default;

bool ListenatorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;

    // mono -> stereo (the cleanup half is mono, the effects half makes the image)
    // and stereo -> stereo
    if (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo())
        return true;

    return in == out;
}

void ListenatorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSr = sampleRate;
    currentBlock = samplesPerBlock;

    const int outCh = juce::jmax (1, getTotalNumOutputChannels());

    analyzer.prepare (sampleRate, samplesPerBlock);
    cleanup.prepare (sampleRate, samplesPerBlock, outCh);
    effects.prepare (sampleRate, samplesPerBlock, outCh);

    monoScratch.setSize (1, samplesPerBlock);

    outputLimiter.prepare (sampleRate, samplesPerBlock, outCh);
    outputLimiter.setThresholdDb (-0.5f);

    setLatencySamples (cleanup.getLatencySamples() + effects.getLatencySamples());
}

void ListenatorProcessor::releaseResources()
{
    cleanup.reset();
    effects.reset();
    outputLimiter.reset();
    analyzer.reset();
}

void ListenatorProcessor::triggerListen()
{
    double bpm = 120.0;
    bool valid = false;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm()) { bpm = *b; valid = true; }

    analysisApplied = false;
    analyzer.startListening (bpm, valid);
}

void ListenatorProcessor::pullParameters()
{
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    auto flag = [&] (const char* id) { return raw (id) > 0.5f; };

    CleanupBypass cb;
    cb.highPass   = flag (pid::bpHighPass);
    cb.deNoise    = flag (pid::bpDeNoise);
    cb.deVerb     = flag (pid::bpDeVerb);
    cb.gate       = flag (pid::bpGate);
    cb.surgicalEq = flag (pid::bpSurgical);
    cb.resonance  = flag (pid::bpResonance);
    cb.deEss      = flag (pid::bpDeEss);
    cb.compressor = flag (pid::bpComp);
    cb.toneMatch  = flag (pid::bpTone);
    cb.limiter    = flag (pid::bpLimiter);
    cb.pitchRepair = flag (pid::bpPitchRepair);

    CleanupTrims ct;
    ct.eqAmount      = raw (pid::eqAmount);
    ct.compAmount    = raw (pid::compAmount);
    ct.deEssAmount   = raw (pid::deEssAmount);
    ct.cleanupAmount = raw (pid::cleanupAmount);

    cleanup.setBypass (cb);
    cleanup.setTrims (ct);

    EffectsBypass eb;
    eb.autoTune   = flag (pid::bpAutoTune);
    eb.doubler    = flag (pid::bpDoubler);
    eb.saturation = flag (pid::bpSaturation);
    eb.exciter    = flag (pid::bpExciter);
    eb.character  = flag (pid::bpCharacter);
    eb.width      = flag (pid::bpWidth);
    eb.delay      = flag (pid::bpDelay);
    eb.reverb     = flag (pid::bpReverb);

    EffectsTrims et;
    et.tuneAmount   = raw (pid::tuneAmount);
    et.doublerMix   = raw (pid::doublerMix);
    et.driveAmount  = raw (pid::driveAmount);
    et.characterMix = raw (pid::characterMix);
    et.widthAmount  = raw (pid::widthAmount);
    et.delayMix     = raw (pid::delayMix);
    et.reverbMix    = raw (pid::reverbMix);
    et.duckAmount   = raw (pid::duckAmount);

    effects.setBypass (eb);
    effects.setTrims (et);
    effects.setKey ((int) raw (pid::keyRoot), (Scale) (int) raw (pid::keyScale));
}

void ListenatorProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();

    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    // mono source feeding a stereo bus: mirror before anything else
    if (totalIn == 1 && totalOut >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    // ---- input metering ----------------------------------------------------
    inputLevelDb.store (juce::Decibels::gainToDecibels (
        buffer.getMagnitude (0, numSamples), -100.0f));

    // ---- feed the LISTEN pass (dry, pre-everything) ------------------------
    if (analyzer.isListening())
    {
        if (monoScratch.getNumSamples() < numSamples)
            monoScratch.setSize (1, numSamples, false, false, true);

        monoScratch.copyFrom (0, 0, buffer, 0, 0, numSamples);
        if (totalIn >= 2)
        {
            monoScratch.addFrom (0, 0, buffer, 1, 0, numSamples);
            monoScratch.applyGain (0.5f);
        }
        analyzer.pushSamples (monoScratch.getReadPointer (0), numSamples);
    }

    // ---- pick up a freshly published analysis ------------------------------
    const int gen = analyzer.getGeneration();
    if (gen != seenGeneration)
    {
        seenGeneration = gen;
        lastResult = analyzer.getResult();
        cleanup.applyAnalysis (lastResult);
        effects.applyAnalysis (lastResult);
        analysisApplied = lastResult.valid;

        setLatencySamples (cleanup.getLatencySamples() + effects.getLatencySamples());
    }

    pullParameters();

    // Cold start: until LISTEN has run, pass audio through untouched.
    if (! analysisApplied)
    {
        outputLevelDb.store (juce::Decibels::gainToDecibels (
            buffer.getMagnitude (0, numSamples), -100.0f));
        return;
    }

    // keep tempo-locked effects following the host
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                effects.setTempo (*b, true);

    const bool skipCleanup = apvts.getRawParameterValue (pid::cleanupBypass)->load() > 0.5f;
    const bool skipEffects = apvts.getRawParameterValue (pid::effectsBypass)->load() > 0.5f;

    if (! skipCleanup) cleanup.process (buffer);
    if (! skipEffects) effects.process (buffer);

    const float outGain = juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue (pid::outputGain)->load());
    buffer.applyGain (outGain);

    // Everything downstream of the cleanup half's limiter -- saturation, the
    // doubler, both sends, the output trim -- can push back over full scale,
    // so the last word belongs here. Skipped when both halves are off, so
    // "bypass everything" really is bit-transparent.
    if (! (skipCleanup && skipEffects))
        outputLimiter.process (buffer);

    outputLevelDb.store (juce::Decibels::gainToDecibels (
        buffer.getMagnitude (0, numSamples), -100.0f));
}

//==============================================================================
juce::AudioProcessorEditor* ListenatorProcessor::createEditor()
{
    return new ListenatorEditor (*this);
}

void ListenatorProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = apvts.copyState().createXml())
        copyXmlToBinary (*state, destData);
}

void ListenatorProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

} // namespace listenator

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new listenator::ListenatorProcessor();
}
