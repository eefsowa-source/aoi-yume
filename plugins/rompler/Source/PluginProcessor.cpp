#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <juce_core/juce_core.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#endif

namespace aod
{

RomplerProcessor::RomplerProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_ (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    setLatencySamples (0);
}

void RomplerProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    sampleRate_ = sampleRate;
    voicePool_ = std::make_unique<VoicePool>();

    // Surface the bundled SoundFont so the plugin starts usable without a
    // manual Load step when the packaged font is present.
    loadBundledSoundFont();

    busProcessor_.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());
    fxProcessor_.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());

    // The oversampling factor is fixed for this prepareToPlay session; see
    // BusProcessor::process for why it cannot change mid-stream without a
    // message-thread call. setLatencySamples() itself is safe to call here —
    // this runs before the host starts pumping audio through processBlock.
    const auto osFactorParam = apvts_.getRawParameterValue (ParamIDs::busOsFactor);
    cachedOsFactorIndex_ = osFactorParam ? static_cast<int> (osFactorParam->load()) : 2;
    setLatencySamples (busProcessor_.getLatencySamples (cachedOsFactorIndex_));
}

void RomplerProcessor::releaseResources()
{
    // The audio thread is guaranteed stopped here, so it is safe to retire
    // all loaders: processBlock() can no longer read activeLoader_.
    activeLoader_.store (nullptr, std::memory_order_relaxed);

    for (auto& slot : sf2Loaders_)
    {
        if (slot)
            retiredLoaders_.push_back (std::move (slot));
    }

    // The audio thread is stopped and every loader has been retired, so the
    // deferred-free list can finally be drained here instead of growing until
    // the plugin instance is destroyed.
    retiredLoaders_.clear();

    bundledFontLoaded_ = false;
    voicePool_.reset();
}

bool RomplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono()
        || output == juce::AudioChannelSet::stereo();
}

void RomplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    // Sync the oversampling factor at the block boundary, before any early
    // return: the reported latency must track the parameter even while no
    // SoundFont is loaded, so the host never sees a stale PDC figure when a
    // font appears mid-session. The BusProcessor keeps all four oversamplers
    // prepared, so switching here never allocates; only the reported latency
    // moves, and setLatencySamples() itself no-ops while the value is
    // unchanged, so the host is notified exactly once per real factor change.
    {
        const auto osFactorParam = apvts_.getRawParameterValue (ParamIDs::busOsFactor);
        const int requestedOsIndex = osFactorParam ? static_cast<int> (osFactorParam->load()) : cachedOsFactorIndex_;
        if (requestedOsIndex != cachedOsFactorIndex_)
        {
            cachedOsFactorIndex_ = requestedOsIndex;
            setLatencySamples (busProcessor_.getLatencySamples (cachedOsFactorIndex_));
        }
    }

    SF2Loader* loader = activeLoader_.load (std::memory_order_acquire);
    if (!voicePool_ || loader == nullptr)
        return;

    float* outL = buffer.getWritePointer(0);
    const int numSamples = buffer.getNumSamples();

    // Drain UI / computer-keyboard note events first (message thread).
    for (;;)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        noteFifo_.prepareToRead (1, start1, size1, start2, size2);
        if (size1 <= 0)
            break;
        const auto [note, on, velocity] = noteBuffer_[static_cast<std::size_t> (start1)];
        noteFifo_.finishedRead (1);
        if (on)
        {
            const int bank = currentBank_.load (std::memory_order_relaxed);
            const int program = currentProgram_.load (std::memory_order_relaxed);
            if (Sample* sample = loader->getSample (bank, program, note, velocity))
                voicePool_->start (sample, note, static_cast<float>(velocity) / 127.0f);
        }
        else
        {
            voicePool_->stop (note);
        }
    }

    for (const auto event : midiMessages)
    {
        const auto msg = event.getMessage();

        if (msg.isNoteOn())
        {
            const int note = msg.getNoteNumber();
            const int velocity = msg.getVelocity();
            const int bank = currentBank_.load (std::memory_order_relaxed);
            const int program = currentProgram_.load (std::memory_order_relaxed);
            Sample* sample = loader->getSample(bank, program, note, velocity);

            if (sample != nullptr)
                voicePool_->start(sample, note, static_cast<float>(velocity) / 127.0f);
        }
        else if (msg.isNoteOff())
        {
            const int note = msg.getNoteNumber();
            voicePool_->stop(note);
        }
        else if (msg.isProgramChange())
        {
            currentProgram_.store (msg.getProgramChangeNumber(), std::memory_order_relaxed);
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            voicePool_->stopAll();
        }
    }

    const auto driveParam = apvts_.getRawParameterValue(ParamIDs::voiceDrive);
    const auto curveParam = apvts_.getRawParameterValue(ParamIDs::voiceCurve);
    const auto velToDriveParam = apvts_.getRawParameterValue(ParamIDs::voiceVelToDrive);
    const auto filterRoutingParam = apvts_.getRawParameterValue(ParamIDs::voiceFilterRouting);
    const auto filterOffsetParam = apvts_.getRawParameterValue(ParamIDs::voiceFilterOffset);
    const auto polyLimitParam = apvts_.getRawParameterValue(ParamIDs::polyLimit);
    const float driveDb = driveParam ? driveParam->load() : 0.0f;
    const int curveId = curveParam ? static_cast<int>(curveParam->load()) : 0;
    const float velToDriveDb = velToDriveParam ? velToDriveParam->load() : 0.0f;
    const int filterRouting = filterRoutingParam ? static_cast<int>(filterRoutingParam->load()) : 0;
    const float filterOffsetCents = filterOffsetParam ? filterOffsetParam->load() : 0.0f;

    if (polyLimitParam)
        voicePool_->setPolyphony (static_cast<int> (polyLimitParam->load()));

    const auto envAttackParam  = apvts_.getRawParameterValue (ParamIDs::envAttack);
    const auto envDecayParam   = apvts_.getRawParameterValue (ParamIDs::envDecay);
    const auto envSustainParam = apvts_.getRawParameterValue (ParamIDs::envSustain);
    const auto envReleaseParam = apvts_.getRawParameterValue (ParamIDs::envRelease);
    const float attackMs     = envAttackParam  ? envAttackParam->load()          : 10.0f;
    const float decayMs      = envDecayParam   ? envDecayParam->load()           : 300.0f;
    const float sustainLevel = envSustainParam ? envSustainParam->load() / 100.0f : 0.7f;
    const float releaseMs    = envReleaseParam ? envReleaseParam->load()         : 80.0f;

    voicePool_->render(outL, numSamples, static_cast<int>(sampleRate_), driveDb, velToDriveDb,
                        curveId, filterRouting, filterOffsetCents,
                        attackMs, decayMs, sustainLevel, releaseMs);

    if (buffer.getNumChannels() > 1)
    {
        float* outR = buffer.getWritePointer(1);
        juce::FloatVectorOperations::copy(outR, outL, numSamples);
    }

    const auto tapeDriveParam = apvts_.getRawParameterValue(ParamIDs::busTapeDrive);
    const auto foldParam = apvts_.getRawParameterValue(ParamIDs::busFold);
    const float tapeDrivePercent = tapeDriveParam ? tapeDriveParam->load() : 0.0f;
    const float foldPercent = foldParam ? foldParam->load() : 0.0f;

    busProcessor_.process (buffer, tapeDrivePercent, foldPercent, cachedOsFactorIndex_);

    const auto chorusRateP  = apvts_.getRawParameterValue (ParamIDs::fxChorusRate);
    const auto chorusDepthP = apvts_.getRawParameterValue (ParamIDs::fxChorusDepth);
    const auto chorusMixP   = apvts_.getRawParameterValue (ParamIDs::fxChorusMix);
    const auto reverbRoomP  = apvts_.getRawParameterValue (ParamIDs::fxReverbRoom);
    const auto reverbDampP  = apvts_.getRawParameterValue (ParamIDs::fxReverbDamp);
    const auto reverbMixP   = apvts_.getRawParameterValue (ParamIDs::fxReverbMix);

    fxProcessor_.process (buffer,
                          chorusRateP  ? chorusRateP->load()  : 1.0f,
                          chorusDepthP ? chorusDepthP->load() / 100.0f : 0.3f,
                          chorusMixP   ? chorusMixP->load()   / 100.0f : 0.25f,
                          reverbRoomP  ? reverbRoomP->load()  / 100.0f : 0.4f,
                          reverbDampP  ? reverbDampP->load()  / 100.0f : 0.5f,
                          reverbMixP   ? reverbMixP->load()   / 100.0f : 0.2f);

    const auto outTrimParam = apvts_.getRawParameterValue(ParamIDs::outTrim);
    const auto outMixParam = apvts_.getRawParameterValue(ParamIDs::outMix);
    const float outTrimDb = outTrimParam ? outTrimParam->load() : 0.0f;
    const float outTrimGain = std::pow(10.0f, outTrimDb / 20.0f);
    const float outMixGain = outMixParam ? outMixParam->load() / 100.0f : 1.0f;

    buffer.applyGain(outTrimGain * outMixGain);

    lastPeak_.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* RomplerProcessor::createEditor()
{
    return new RomplerEditor (*this);
}

void RomplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = std::make_unique<juce::XmlElement> ("EONDS50State");

    // Save APVTS parameters.
    if (auto paramXml = apvts_.copyState().createXml())
        xml->addChildElement (paramXml.release());

    // Save bank slot state.
    auto* banksXml = xml->createNewChildElement ("Banks");
    banksXml->setAttribute ("activeSlot", activeBankSlot_.load (std::memory_order_relaxed));
    for (int i = 0; i < maxBanks; ++i)
    {
        auto* slotXml = banksXml->createNewChildElement ("Slot");
        slotXml->setAttribute ("index", i);
        slotXml->setAttribute ("file", bankNames_[static_cast<std::size_t> (i)]);
        slotXml->setAttribute ("path", bankPaths_[static_cast<std::size_t> (i)]);
    }

    copyXmlToBinary (*xml, destData);
}

void RomplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (! xml)
        return;

    // Restore APVTS parameters.
    if (auto* paramXml = xml->getChildByName (apvts_.state.getType()))
        apvts_.replaceState (juce::ValueTree::fromXml (*paramXml));

    // Restore bank slots.
    if (auto* banksXml = xml->getChildByName ("Banks"))
    {
        const int activeSlot = banksXml->getIntAttribute ("activeSlot", 0);
        for (auto* slotXml : banksXml->getChildIterator())
        {
            if (! slotXml->hasTagName ("Slot"))
                continue;
            const int idx = slotXml->getIntAttribute ("index", -1);
            // Prefer the full path; fall back to the legacy filename attribute
            // which only ever resolves when the host's CWD happens to match.
            const auto fileName = slotXml->getStringAttribute ("path",
                                  slotXml->getStringAttribute ("file", {}));
            if (idx >= 0 && idx < maxBanks && fileName.isNotEmpty())
            {
                const juce::File file (fileName);
                if (file.existsAsFile())
                    loadSoundFont (file, idx);
            }
        }
        switchBank (juce::jlimit (0, maxBanks - 1, activeSlot));
    }
}

void RomplerProcessor::loadSoundFont(const juce::File& file)
{
    loadSoundFont (file, activeBankSlot_.load (std::memory_order_relaxed));
}

void RomplerProcessor::loadSoundFont(const juce::File& file, int bankSlot)
{
    if (bankSlot < 0 || bankSlot >= maxBanks)
        return;

    auto newLoader = std::make_unique<SF2Loader>(static_cast<int>(sampleRate_));
    if (!newLoader->loadFile(file))
        return;

    bankNames_[static_cast<std::size_t> (bankSlot)] = file.getFileName();
    bankPaths_[static_cast<std::size_t> (bankSlot)] = file.getFullPathName();

    // If this is the active slot, update currentBank/currentProgram and publish.
    if (bankSlot == activeBankSlot_.load (std::memory_order_relaxed))
    {
        const auto [bank, program] = newLoader->firstPresetProgram();
        currentBank_.store (bank, std::memory_order_relaxed);
        currentProgram_.store (program, std::memory_order_relaxed);
        activeLoader_.store (newLoader.get(), std::memory_order_release);
    }

    // Retire the old loader for this slot.
    auto& slot = sf2Loaders_[static_cast<std::size_t> (bankSlot)];
    if (slot)
        retiredLoaders_.push_back (std::move (slot));
    slot = std::move (newLoader);
}

void RomplerProcessor::removeBank(int bankSlot)
{
    if (bankSlot < 0 || bankSlot >= maxBanks)
        return;

    auto& slot = sf2Loaders_[static_cast<std::size_t> (bankSlot)];
    if (slot)
    {
        // If removing the active slot, clear activeLoader_ first.
        if (bankSlot == activeBankSlot_.load (std::memory_order_relaxed))
        {
            activeLoader_.store (nullptr, std::memory_order_release);
            currentBank_.store (0, std::memory_order_relaxed);
            currentProgram_.store (0, std::memory_order_relaxed);
        }
        retiredLoaders_.push_back (std::move (slot));
    }
    bankNames_[static_cast<std::size_t> (bankSlot)] = {};
    bankPaths_[static_cast<std::size_t> (bankSlot)] = {};
}

void RomplerProcessor::switchBank(int bankSlot)
{
    if (bankSlot < 0 || bankSlot >= maxBanks)
        return;

    activeBankSlot_.store (bankSlot, std::memory_order_relaxed);

    auto* loader = sf2Loaders_[static_cast<std::size_t> (bankSlot)].get();
    activeLoader_.store (loader, std::memory_order_release);

    if (loader)
    {
        const auto [bank, program] = loader->firstPresetProgram();
        currentBank_.store (bank, std::memory_order_relaxed);
        currentProgram_.store (program, std::memory_order_relaxed);
    }
    else
    {
        currentBank_.store (0, std::memory_order_relaxed);
        currentProgram_.store (0, std::memory_order_relaxed);
    }
}

bool RomplerProcessor::isBankLoaded(int bankSlot) const noexcept
{
    if (bankSlot < 0 || bankSlot >= maxBanks)
        return false;
    return sf2Loaders_[static_cast<std::size_t> (bankSlot)] != nullptr;
}

// ---------------------------------------------------------------------------
// Bundled SoundFont
// ---------------------------------------------------------------------------
//
// At build time the packaged VST3/AU bundle is given an .sf2 alongside its
// moduleinfo.json, inside Contents/Resources. On macOS the plugin binary lives
// at <bundle>/Contents/MacOS/<name>, so the Resources sibling is found by
// walking up. The bundled font is offered during prepareToPlay() so a host that
// loads the plugin is immediately playable without a manual Load step.
namespace
{
    juce::File pathForBundledSoundFont()
    {
#if JUCE_MAC
        // Locate this module's own path with dladdr and walk up from
        // <bundle>/Contents/MacOS/<name> to <bundle>/Contents/Resources. This
        // works in every host because it does not depend on which bundle the
        // host considers "main".
        Dl_info info;
        if (dladdr (reinterpret_cast<void*> (&pathForBundledSoundFont), &info) == 0
            || info.dli_fname == nullptr)
            return {};

        juce::File resourcesDir = juce::File (juce::String (info.dli_fname))
                                      .getParentDirectory()   // Contents/MacOS
                                      .getParentDirectory()   // Contents
                                      .getChildFile ("Resources");
        for (auto& entry : juce::RangedDirectoryIterator (resourcesDir, false))
            if (entry.getFile().hasFileExtension (".sf2"))
                return entry.getFile();

        return {};
#else
        // Non-macOS packaging does not yet bundle an SF2.
        return {};
#endif
    }
} // namespace

void RomplerProcessor::loadBundledSoundFont()
{
    if (bundledFontLoaded_)
        return;

    const juce::File file = pathForBundledSoundFont();
    if (file.existsAsFile())
    {
        loadSoundFont (file, 0);
        bundledFontLoaded_ = isBankLoaded (0);
    }
}

int RomplerProcessor::getPresetCount() const noexcept
{
    const int slot = activeBankSlot_.load (std::memory_order_relaxed);
    auto* loader = sf2Loaders_[static_cast<std::size_t> (slot)].get();
    return loader ? loader->presetCount() : 0;
}

juce::String RomplerProcessor::getPresetName (int presetIndex) const noexcept
{
    const int slot = activeBankSlot_.load (std::memory_order_relaxed);
    auto* loader = sf2Loaders_[static_cast<std::size_t> (slot)].get();
    return loader ? loader->presetName (presetIndex) : juce::String {};
}

std::pair<int, int> RomplerProcessor::getPresetBankProgram (int presetIndex) const noexcept
{
    const int slot = activeBankSlot_.load (std::memory_order_relaxed);
    auto* loader = sf2Loaders_[static_cast<std::size_t> (slot)].get();
    return loader ? loader->presetBankProgram (presetIndex) : std::pair<int, int> { 0, 0 };
}

void RomplerProcessor::selectPreset (int bank, int program) noexcept
{
    currentBank_.store (bank, std::memory_order_relaxed);
    currentProgram_.store (program, std::memory_order_relaxed);
}

void RomplerProcessor::postNote (int note, bool on, int velocity)
{
    // When the queue is full we drop the *new* event rather than the oldest.
    // Dropping the oldest strands a note-on without its matching note-off (or
    // vice versa): a lost note-on leaves the note silent, but a lost note-off
    // leaves it ringing forever. Both are dropped symmetrically here, so the
    // worst case is a momentarily missed keypress, never a stuck note.
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    noteFifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 <= 0)
        return;
    noteBuffer_[static_cast<std::size_t> (start1)] = { note, on, velocity };
    noteFifo_.finishedWrite (1);
}

} // namespace aod

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new aod::RomplerProcessor();
}
