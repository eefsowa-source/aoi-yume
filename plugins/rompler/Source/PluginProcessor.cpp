#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include "PluginEditor.h"

#include <juce_core/juce_core.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#endif

namespace aod
{

namespace
{
    // Initial synthetic spread for mono SF2 voices. Keep this internal until
    // a user-facing parameter is approved; the existing CC10 pan remains the
    // global image control.
    constexpr float kInitialVoiceStereoWidth = 0.60f;
}

RomplerProcessor::RomplerProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_ (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    setLatencySamples (0);
    startTimerHz (60);
}

RomplerProcessor::~RomplerProcessor()
{
    stopTimer();
    // Drain all remaining retired loaders to prevent dangling SF2Loader
    // instances. releaseResources() has already moved all active loaders
    // to retiredLoaders_, so this completes the final cleanup.
    drainRetiredLoaders();
}

void RomplerProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    sampleRate_ = sampleRate;
    voicePool_ = std::make_unique<VoicePool> (VoicePool::maxVoices);
    voicePool_->prepare (maximumExpectedSamplesPerBlock);

    // Some hosts call prepareToPlay again without releaseResources(). Keep
    // loaded banks matched to the new rate, but do the work on the message
    // thread: prepareToPlay runs on a host-chosen thread where loadSoundFont()
    // is not safe to call, and a rate change means file I/O plus a resample
    // pass per bank. The flag re-arms on every prepare, so the drain
    // converges every slot to the most recently requested rate; a
    // block-size-only change reloads nothing.
    pendingBankReloadRate_.store (static_cast<int> (sampleRate), std::memory_order_release);
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        manager == nullptr || manager->isThisTheMessageThread())
    {
        // Either this is already the message thread (where loadSoundFont()
        // is legal anyway) or no pumpable message loop exists at all —
        // offline renderers and headless hosts would leave a deferred flag
        // set forever, so run the bank work inline here.
        reloadBanksForPreparedRate();
        loadBundledSoundFont();
    }
    else
    {
        queueAsyncFlag (reloadBanksForRate | offerBundledFont);
    }

    busProcessor_.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());
    dynamicsProcessor_.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());
    outputSafetyProcessor_.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());
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

    bundledFontLoaded_.store (false, std::memory_order_relaxed);
    voicePool_.reset();
    dynamicsProcessor_.reset();
    
    // Drain all retired loaders now that the audio thread is stopped.
    // This ensures no dangling SF2Loader instances remain.
    drainRetiredLoaders();
}

bool RomplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono()
        || output == juce::AudioChannelSet::stereo();
}

void RomplerProcessor::syncBlockParameters() noexcept
{
    const auto value = [this] (const char* id, float fallback)
    {
        if (const auto* parameter = apvts_.getRawParameterValue (id))
            return parameter->load();
        return fallback;
    };

    const int requestedOsIndex = static_cast<int> (value (ParamIDs::busOsFactor,
                                                           static_cast<float> (cachedOsFactorIndex_)));
    if (requestedOsIndex != cachedOsFactorIndex_)
    {
        cachedOsFactorIndex_ = requestedOsIndex;
        pendingLatencySamples_.store (busProcessor_.getLatencySamples (cachedOsFactorIndex_),
                                      std::memory_order_release);
        queueAsyncFlag (mirrorLatency);
    }

    // A captured CC65 is authoritative until its deferred APVTS mirror lands;
    // re-reading the parameter before then would restore the pre-controller
    // value and drop legato after a single block. Same policy as CC71/74.
    if (legatoCcGeneration_.load (std::memory_order_acquire)
        == legatoMirroredGeneration_.load (std::memory_order_acquire))
    {
        const int requestedLegato = static_cast<int> (value (ParamIDs::voiceLegato,
                                                              static_cast<float> (cachedLegatoParamValue_)));
        if (requestedLegato != cachedLegatoParamValue_)
        {
            cachedLegatoParamValue_ = requestedLegato;
            const bool enabled = requestedLegato != 0;
            legatoEnabled_.store (enabled, std::memory_order_relaxed);
            if (voicePool_)
                voicePool_->setLegatoEnabled (enabled);
        }
    }

    blockParameters_.driveDb = value (ParamIDs::voiceDrive, 0.0f);
    blockParameters_.curveId = static_cast<int> (value (ParamIDs::voiceCurve, 0.0f));
    blockParameters_.velToDriveDb = value (ParamIDs::voiceVelToDrive, 0.0f);
    blockParameters_.filterRouting = static_cast<int> (value (ParamIDs::voiceFilterRouting, 0.0f));
    if (filterOffsetCcGeneration_.load (std::memory_order_acquire)
        == filterOffsetMirroredGeneration_.load (std::memory_order_acquire))
        realtimeFilterOffsetCents_.store (value (ParamIDs::voiceFilterOffset, 0.0f), std::memory_order_relaxed);
    blockParameters_.filterOffsetCents = realtimeFilterOffsetCents_.load (std::memory_order_relaxed);
    blockParameters_.polyphony = static_cast<int> (value (ParamIDs::polyLimit,
                                                           static_cast<float> (VoicePool::maxVoices)));
    blockParameters_.attackMs = value (ParamIDs::envAttack, 0.0f);
    blockParameters_.decayMs = value (ParamIDs::envDecay, 0.0f);
    // APVTS exposes sustain as a user-facing percentage; DSP ADSR expects a
    // normalized 0..1 level.
    blockParameters_.sustainLevel = value (ParamIDs::envSustain, 100.0f) / 100.0f;
    blockParameters_.releaseMs = value (ParamIDs::envRelease, 0.0f);
    blockParameters_.pitchBendSemitones = pitchBendSemitones_.load (std::memory_order_relaxed);
    blockParameters_.vibratoDepthCents = modWheelValue_.load (std::memory_order_relaxed) * kMaxVibratoDepthCents;
    blockParameters_.ccGain = masterVolumeCc7_ * expressionCc11_;
    blockParameters_.pan = panCc10_;
    blockParameters_.tapeDrivePercent = value (ParamIDs::busTapeDrive, 0.0f);
    blockParameters_.foldPercent = value (ParamIDs::busFold, 0.0f);
    if (busCutoffCcGeneration_.load (std::memory_order_acquire)
        == busCutoffMirroredGeneration_.load (std::memory_order_acquire))
        realtimeBusCutoffHz_.store (value (ParamIDs::busFilterCutoff, 20000.0f), std::memory_order_relaxed);
    blockParameters_.filterCutoffHz = realtimeBusCutoffHz_.load (std::memory_order_relaxed);
    blockParameters_.filterResonancePercent = value (ParamIDs::busFilterResonance, 0.0f);
    blockParameters_.compThreshold = value (ParamIDs::compThreshold, -18.0f);
    blockParameters_.compRatio = value (ParamIDs::compRatio, 3.0f);
    blockParameters_.compAttack = value (ParamIDs::compAttack, 15.0f);
    blockParameters_.compRelease = value (ParamIDs::compRelease, 180.0f);
    blockParameters_.compMakeup = value (ParamIDs::compMakeup, 0.0f);
    blockParameters_.compMix = value (ParamIDs::compMix, 0.0f);
    blockParameters_.chorusRate = value (ParamIDs::fxChorusRate, 1.0f);
    blockParameters_.chorusDepth = value (ParamIDs::fxChorusDepth, 30.0f) / 100.0f;
    blockParameters_.chorusMix = value (ParamIDs::fxChorusMix, 25.0f) / 100.0f;
    blockParameters_.reverbRoom = value (ParamIDs::fxReverbRoom, 40.0f) / 100.0f;
    blockParameters_.reverbDamp = value (ParamIDs::fxReverbDamp, 50.0f) / 100.0f;
    blockParameters_.reverbMix = value (ParamIDs::fxReverbMix, 20.0f) / 100.0f;
    blockParameters_.delayMix = value (ParamIDs::fxDelayMix, 0.0f) / 100.0f;
    blockParameters_.delayFeedback = value (ParamIDs::fxDelayFeedback, 35.0f) / 100.0f;
    blockParameters_.outputGain = std::pow (10.0f, value (ParamIDs::outTrim, -3.0f) / 20.0f)
                                * value (ParamIDs::outMix, 100.0f) / 100.0f;
    blockParameters_.bpm = 120.0f;
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
            if (const auto bpm = position->getBpm())
                blockParameters_.bpm = static_cast<float> (*bpm);

    if (voicePool_)
        voicePool_->setPolyphony (blockParameters_.polyphony);
}

void RomplerProcessor::dispatchUiNote (const UiNoteEvent& event, const SF2Loader& loader) noexcept
{
    if (event.noteOn)
    {
        const int bank = currentBank_.load (std::memory_order_relaxed);
        const int program = currentProgram_.load (std::memory_order_relaxed);
        std::array<const Sample*, SF2Loader::maxMatchingSamples> samples {};
        const auto resolved = loader.getSamples (bank, program, event.note, event.velocity, samples);
        const auto layerCount = std::min (resolved, samples.size());
        if (layerCount != 0)
        {
            voicePool_->start (std::span<const Sample* const> { samples.data(), layerCount }, event.note,
                               static_cast<float> (event.velocity) / 127.0f, &loader);
            // Every layer must hold the token that keeps its decoded sample
            // storage alive across deferred SoundFont retirement.
            const int activeBankSlot = activeBankSlot_.load (std::memory_order_relaxed);
            BankToken token;
            token.bankSlot = activeBankSlot;
            token.generation = bankGeneration_[static_cast<std::size_t>(activeBankSlot)].load (std::memory_order_acquire);
            token.bankId = bankFileHash_[static_cast<std::size_t>(activeBankSlot)];
            voicePool_->setBankTokenForNote (event.note, token);
        }
    }
    else
    {
        voicePool_->stop (event.note);
    }
}

void RomplerProcessor::dispatchMidiMessage (const juce::MidiMessage& msg, const SF2Loader& loader) noexcept
{
    if (msg.isNoteOn())
    {
        const int bank = currentBank_.load (std::memory_order_relaxed);
        const int program = currentProgram_.load (std::memory_order_relaxed);
        std::array<const Sample*, SF2Loader::maxMatchingSamples> samples {};
        const auto resolved = loader.getSamples (bank, program, msg.getNoteNumber(), msg.getVelocity(), samples);
        const auto layerCount = std::min (resolved, samples.size());
        if (layerCount != 0)
        {
            voicePool_->start (std::span<const Sample* const> { samples.data(), layerCount }, msg.getNoteNumber(),
                               static_cast<float> (msg.getVelocity()) / 127.0f, &loader);
            // The same note may own several SF2 zones, all of which need this
            // generation token while a retired bank waits for its voices.
            const int activeBankSlot = activeBankSlot_.load (std::memory_order_relaxed);
            BankToken token;
            token.bankSlot = activeBankSlot;
            token.generation = bankGeneration_[static_cast<std::size_t>(activeBankSlot)].load (std::memory_order_acquire);
            token.bankId = bankFileHash_[static_cast<std::size_t>(activeBankSlot)];
            voicePool_->setBankTokenForNote (msg.getNoteNumber(), token);
        }
    }
    else if (msg.isNoteOff())
    {
        voicePool_->stop (msg.getNoteNumber());
    }
    else if (msg.isProgramChange())
    {
        const int program = msg.getProgramChangeNumber();
        currentProgram_.store (program, std::memory_order_relaxed);
        if (program >= 0 && program < 8)
            requestQuickSlot (program + 1);
    }
    else if (msg.isPitchWheel())
    {
        const float normalized = (static_cast<float> (msg.getPitchWheelValue()) - 8192.0f) / 8192.0f;
        blockParameters_.pitchBendSemitones = juce::jlimit (-1.0f, 1.0f, normalized) * kPitchBendRangeSemitones;
        pitchBendSemitones_.store (blockParameters_.pitchBendSemitones, std::memory_order_relaxed);
    }
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        voicePool_->stopAll();
    }
    else if (msg.isController())
    {
        const int ccNumber = msg.getControllerNumber();
        const int ccValue = msg.getControllerValue();
        const float normalizedCc = static_cast<float> (ccValue) / 127.0f;
        switch (ccNumber)
        {
            case 1:
                modWheelValue_.store (normalizedCc, std::memory_order_relaxed);
                blockParameters_.vibratoDepthCents = normalizedCc * kMaxVibratoDepthCents;
                break;
            case 7:
                masterVolumeCc7_ = normalizedCc;
                blockParameters_.ccGain = masterVolumeCc7_ * expressionCc11_;
                break;
            case 10:
                panCc10_ = normalizedCc;
                blockParameters_.pan = panCc10_;
                break;
            case 11:
                expressionCc11_ = normalizedCc;
                blockParameters_.ccGain = masterVolumeCc7_ * expressionCc11_;
                break;
            case 64:
            {
                const bool held = ccValue >= 64;
                sustainHeld_.store (held, std::memory_order_relaxed);
                voicePool_->setSustainHeld (held);
                break;
            }
            case 65:
            {
                const bool enabled = ccValue >= 64;
                legatoEnabled_.store (enabled, std::memory_order_relaxed);
                voicePool_->setLegatoEnabled (enabled);
                cachedLegatoParamValue_ = enabled ? 1 : 0;
                legatoCcGeneration_.fetch_add (1, std::memory_order_acq_rel);
                pendingLegatoNormalized_.store (enabled ? 1.0f : 0.0f, std::memory_order_release);
                legatoCcGeneration_.fetch_add (1, std::memory_order_release);
                queueAsyncFlag (mirrorLegato);
                break;
            }
            case 71:
                if (auto* param = apvts_.getParameter (ParamIDs::voiceFilterOffset))
                {
                    const auto filterOffset = param->convertFrom0to1 (normalizedCc);
                    realtimeFilterOffsetCents_.store (filterOffset, std::memory_order_relaxed);
                    blockParameters_.filterOffsetCents = filterOffset;
                    filterOffsetCcGeneration_.fetch_add (1, std::memory_order_acq_rel);
                    pendingFilterOffsetNormalized_.store (normalizedCc, std::memory_order_release);
                    filterOffsetCcGeneration_.fetch_add (1, std::memory_order_release);
                    queueAsyncFlag (mirrorFilterOffset);
                }
                break;
            case 74:
                if (auto* param = apvts_.getParameter (ParamIDs::busFilterCutoff))
                {
                    const auto busCutoff = param->convertFrom0to1 (normalizedCc);
                    realtimeBusCutoffHz_.store (busCutoff, std::memory_order_relaxed);
                    blockParameters_.filterCutoffHz = busCutoff;
                    busCutoffCcGeneration_.fetch_add (1, std::memory_order_acq_rel);
                    pendingBusCutoffNormalized_.store (normalizedCc, std::memory_order_release);
                    busCutoffCcGeneration_.fetch_add (1, std::memory_order_release);
                    queueAsyncFlag (mirrorBusCutoff);
                }
                break;
            default:
                break;
        }
    }
}

void RomplerProcessor::renderRange (juce::AudioBuffer<float>& buffer, int start, int count) noexcept
{
    if (count <= 0)
        return;

    std::array<float*, 2> channels {};
    const int numChannels = buffer.getNumChannels();
    for (int channel = 0; channel < numChannels; ++channel)
        channels[static_cast<std::size_t> (channel)] = buffer.getWritePointer (channel) + start;
    juce::AudioBuffer<float> range (channels.data(), numChannels, count);

    float* outL = range.getWritePointer (0);
    float* outR = numChannels > 1 ? range.getWritePointer (1) : nullptr;
    voicePool_->renderStereo (outL, outR, count, static_cast<int> (sampleRate_),
                              blockParameters_.driveDb, blockParameters_.velToDriveDb,
                              blockParameters_.curveId, blockParameters_.filterRouting,
                              blockParameters_.filterOffsetCents, blockParameters_.attackMs,
                              blockParameters_.decayMs, blockParameters_.sustainLevel,
                              blockParameters_.releaseMs, blockParameters_.pitchBendSemitones,
                              blockParameters_.vibratoDepthCents, kInitialVoiceStereoWidth);

    if (blockParameters_.ccGain != 1.0f)
    {
        juce::FloatVectorOperations::multiply (outL, blockParameters_.ccGain, count);
        if (outR != nullptr)
            juce::FloatVectorOperations::multiply (outR, blockParameters_.ccGain, count);
    }

    if (outR != nullptr)
    {
        if (blockParameters_.pan != 0.5f)
        {
            const float pan = juce::jlimit (0.0f, 1.0f, blockParameters_.pan);
            juce::FloatVectorOperations::multiply (outL, std::sin ((1.0f - pan) * juce::MathConstants<float>::halfPi), count);
            juce::FloatVectorOperations::multiply (outR, std::sin (pan * juce::MathConstants<float>::halfPi), count);
        }
    }

    busProcessor_.process (range, blockParameters_.tapeDrivePercent, blockParameters_.foldPercent,
                           blockParameters_.filterCutoffHz, blockParameters_.filterResonancePercent,
                           cachedOsFactorIndex_);
    dynamicsProcessor_.process (range, blockParameters_.compThreshold, blockParameters_.compRatio,
                                blockParameters_.compAttack, blockParameters_.compRelease,
                                blockParameters_.compMakeup, blockParameters_.compMix);
    fxProcessor_.process (range, blockParameters_.chorusRate, blockParameters_.chorusDepth,
                          blockParameters_.chorusMix, blockParameters_.reverbRoom,
                          blockParameters_.reverbDamp, blockParameters_.reverbMix,
                          blockParameters_.delayMix, blockParameters_.delayFeedback,
                          blockParameters_.bpm);
    range.applyGain (blockParameters_.outputGain);
    outputSafetyProcessor_.process (range);

    // Publish which notes still own a sounding voice so the editor keybed can
    // mirror live MIDI input. Runs once per rendered range: voices that end
    // inside this range drop out on the next segment's publish.
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    const int capacity = voicePool_->preparedCapacity();
    for (int index = 0; index < capacity; ++index)
        if (const Voice* voice = voicePool_->getVoiceAtIndex (index);
            voice != nullptr && voice->isActive() && voice->note() >= 0)
        {
            if (voice->note() < 64)
                lo |= std::uint64_t { 1 } << static_cast<unsigned> (voice->note());
            else
                hi |= std::uint64_t { 1 } << static_cast<unsigned> (voice->note() - 64);
        }
    activeNotesLo_.store (lo, std::memory_order_relaxed);
    activeNotesHi_.store (hi, std::memory_order_relaxed);
}

void RomplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    struct AudioBlockReadScope
    {
        explicit AudioBlockReadScope (std::atomic<std::uint32_t>& activeBlocks) noexcept
            : activeBlocks_ (activeBlocks)
        {
            activeBlocks_.fetch_add (1, std::memory_order_seq_cst);
        }

        ~AudioBlockReadScope()
        {
            activeBlocks_.fetch_sub (1, std::memory_order_seq_cst);
        }

        AudioBlockReadScope (const AudioBlockReadScope&) = delete;
        AudioBlockReadScope& operator= (const AudioBlockReadScope&) = delete;

    private:
        std::atomic<std::uint32_t>& activeBlocks_;
    };

    const AudioBlockReadScope audioBlockReadScope (audioBlocksInFlight_);
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    syncBlockParameters();

    const SF2Loader* loader = activeLoader_.load (std::memory_order_seq_cst);
    if (! voicePool_)
        return;

    const bool queueOverflowed = noteQueueOverflowed_.exchange (false, std::memory_order_acq_rel);
    UiNoteEvent uiNote {};
    for (std::size_t drained = 0; drained < maxQueuedNotes && noteQueue_.tryPop (uiNote); ++drained)
    {
        if (loader != nullptr)
            dispatchUiNote (uiNote, *loader);
        else if (! uiNote.noteOn)
            voicePool_->stop (uiNote.note);
    }

    if (queueOverflowed)
    {
        voicePool_->stopAll();
        uiNoteQueueOverflowDiagnostic_.store (true, std::memory_order_release);
    }

    if (loader == nullptr)
        return;

    const int blockSamples = buffer.getNumSamples();
    int cursor = 0;
    for (const auto metadata : midiMessages)
    {
        const int position = juce::jlimit (0, blockSamples, metadata.samplePosition);
        renderRange (buffer, cursor, position - cursor);
        dispatchMidiMessage (metadata.getMessage(), *loader);
        cursor = position;
    }
    renderRange (buffer, cursor, blockSamples - cursor);
    lastPeak_.store (buffer.getMagnitude (0, blockSamples), std::memory_order_relaxed);
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
            if (slotXml->getTagName() != "Slot")
            continue;
            const int idx = slotXml->getIntAttribute ("index", -1);
            const auto fileName = slotXml->getStringAttribute ("file", {});
            if (idx >= 0 && idx < maxBanks && fileName.isNotEmpty())
            {
                juce::File file (fileName);
                // Older state blobs stored only a filename. Keep accepting
                // those documents by checking the current directory first,
                // then the packaged SoundFonts directory.
                if (! file.existsAsFile())
                {
                    const auto bundledName = canonicalBundledSoundFontFileName (file.getFileName());
                    file = getBundledSoundFontsDirectory().getChildFile (bundledName);
                }
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

    auto newLoader = SF2Loader::loadCached (file, static_cast<int> (sampleRate_));
    if (! newLoader)
        return;

    // Keep the absolute identity so a captured preset can be restored even
    // when the working directory or host process changes.
    bankNames_[static_cast<std::size_t> (bankSlot)] = file.getFullPathName();

    // If this is the active slot, update currentBank/currentProgram and publish.
    // Increment generation and store file hash for bank token
    const auto fileHash = std::hash<std::string>{}(file.getFullPathName().toStdString());
    bankFileHash_[static_cast<std::size_t>(bankSlot)] = fileHash;
    bankGeneration_[static_cast<std::size_t>(bankSlot)].fetch_add(1, std::memory_order_release);

    if (bankSlot == activeBankSlot_.load (std::memory_order_relaxed))
    {
        const auto [bank, program] = newLoader->firstPresetProgram();
        currentBank_.store (bank, std::memory_order_relaxed);
        currentProgram_.store (program, std::memory_order_relaxed);
        activeLoader_.store (newLoader.get(), std::memory_order_seq_cst);
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
            activeLoader_.store (nullptr, std::memory_order_seq_cst);
            currentBank_.store (0, std::memory_order_relaxed);
            currentProgram_.store (0, std::memory_order_relaxed);
        }
        retiredLoaders_.push_back (std::move (slot));
    }
    bankNames_[static_cast<std::size_t> (bankSlot)] = {};
}

void RomplerProcessor::switchBank(int bankSlot)
{
    if (bankSlot < 0 || bankSlot >= maxBanks)
        return;

    activeBankSlot_.store (bankSlot, std::memory_order_relaxed);

    auto* loader = sf2Loaders_[static_cast<std::size_t> (bankSlot)].get();
    activeLoader_.store (loader, std::memory_order_seq_cst);

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

juce::String RomplerProcessor::canonicalBundledSoundFontFileName (const juce::String& fileName)
{
    if (fileName == "Sonic_Mania_-_Korg_M1_Legacy_Soundfont.sf2")
        return "Crystal Legacy.sf2";
    if (fileName == "Live HQ Natural SoundFont GM.sf2")
        return "Natural Stage.sf2";
    if (fileName == "SGM-v2.01-NicePianosGuitarsBass-V1.2.sf2")
        return "Studio Essentials.sf2";
    return fileName;
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
    /** The exact filename picked as the startup default; must stay in sync
        with the bundling logic in plugins/rompler/CMakeLists.txt. */
    constexpr const char* kDefaultBundledSoundFontName = "Crystal Legacy.sf2";

#if JUCE_MAC
    /** <bundle>/Contents/Resources, located by walking up from this module's
        own on-disk path via dladdr. Works regardless of which bundle the
        host considers "main". Returns an invalid File outside a bundle. */
    juce::File resourcesDirectory()
    {
        Dl_info info;
        if (dladdr (reinterpret_cast<void*> (&resourcesDirectory), &info) == 0
            || info.dli_fname == nullptr)
            return {};

        return juce::File (juce::String (info.dli_fname))
                   .getParentDirectory()   // Contents/MacOS
                   .getParentDirectory()   // Contents
                   .getChildFile ("Resources");
    }
#endif

    /** Contents/Resources/SoundFonts, the folder the bundled SF2s are copied
        into by CMakeLists.txt. Non-macOS packaging does not yet bundle SF2s. */
    juce::File soundFontsDirectory()
    {
#if JUCE_MAC
        return resourcesDirectory().getChildFile ("SoundFonts");
#else
        return {};
#endif
    }

    /** The startup-default SF2, matched by exact filename (not "first .sf2
        found") so the deterministic default survives alongside the other
        bundled fonts in the same folder. */
    juce::File pathForBundledSoundFont()
    {
        const juce::File file = soundFontsDirectory().getChildFile (kDefaultBundledSoundFontName);
        return file.existsAsFile() ? file : juce::File {};
    }
} // namespace

void RomplerProcessor::loadBundledSoundFont()
{
    // Never replace a bank that is already in slot 0: the offer now runs on
    // the message-thread drain after prepareToPlay(), and a session restore
    // or explicit load can legitimately land in between.
    if (bundledFontLoaded_.load (std::memory_order_relaxed) || sf2Loaders_[0])
        return;

    const juce::File file = pathForBundledSoundFont();
    if (file.existsAsFile())
    {
        loadSoundFont (file, 0);
        bundledFontLoaded_.store (isBankLoaded (0), std::memory_order_relaxed);
    }
}

juce::File RomplerProcessor::getBundledSoundFontsDirectory() const noexcept
{
    return soundFontsDirectory();
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

    for (const auto* id : ParamSets::rotary)
        if (auto* parameter = apvts_.getParameter (id))
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
}

namespace
{
void captureParameter (const juce::AudioProcessorValueTreeState& state,
                       const char* id, bool isChoice, juce::String choiceText,
                       std::vector<PresetParameterValue>& destination)
{
    if (const auto* parameter = state.getParameter (id))
    {
        PresetParameterValue value;
        value.id = id;
        value.isChoice = isChoice;
        value.value = parameter->getValue();
        value.text = std::move (choiceText);
        destination.push_back (std::move (value));
    }
}

juce::String currentChoiceText (const juce::AudioProcessorValueTreeState& state,
                                const char* id, const juce::StringArray& choices)
{
    if (const auto* parameter = state.getParameter (id))
    {
        const auto normalised = juce::jlimit (0.0f, 1.0f, parameter->getValue());
        const auto index = juce::jlimit (0, choices.size() - 1,
                                         juce::roundToInt (normalised * static_cast<float> (choices.size() - 1)));
        return choices[index];
    }
    return {};
}

void applyChoice (juce::AudioProcessorValueTreeState& state,
                  const PresetParameterValue& stored,
                  const juce::StringArray& choices)
{
    if (auto* parameter = state.getParameter (stored.id))
    {
        const int index = choices.indexOf (stored.text);
        if (index >= 0 && choices.size() > 1)
            parameter->setValueNotifyingHost (static_cast<float> (index) / static_cast<float> (choices.size() - 1));
    }
}
} // namespace

PresetDocument RomplerProcessor::capturePreset() const
{
    PresetDocument document;
    document.name = "Untitled";
    document.source = PresetSource::user;
    document.createdAtMs = juce::Time::getCurrentTime().toMilliseconds();
    document.modifiedAtMs = document.createdAtMs;

    const int slot = activeBankSlot_.load (std::memory_order_relaxed);
    document.soundFontPath = bankNames_[static_cast<std::size_t> (slot)];
    document.soundFontName = juce::File (document.soundFontPath).getFileName();
    document.bank = currentBank_.load (std::memory_order_relaxed);
    document.program = currentProgram_.load (std::memory_order_relaxed);

    for (const auto* id : ParamSets::rotary)
        captureParameter (apvts_, id, false, {}, document.parameters);

    captureParameter (apvts_, ParamIDs::voiceCurve, true,
                      currentChoiceText (apvts_, ParamIDs::voiceCurve, Choices::curve), document.parameters);
    captureParameter (apvts_, ParamIDs::voiceFilterRouting, true,
                      currentChoiceText (apvts_, ParamIDs::voiceFilterRouting, Choices::filterRouting), document.parameters);
    captureParameter (apvts_, ParamIDs::voiceLegato, true,
                      currentChoiceText (apvts_, ParamIDs::voiceLegato, Choices::legato), document.parameters);
    return document;
}

RomplerProcessor::ApplyStatus RomplerProcessor::applyPreset (const PresetDocument& document, const juce::File& resolvedSoundFont)
{
    if (! resolvedSoundFont.existsAsFile())
        return ApplyStatus::soundFontMissing;

    // PresetDocument::bank is the SoundFont MIDI bank, not our internal
    // multi-bank slot. Replace the currently selected slot, then restore the
    // document's bank/program pair after the loader has published.
    const int bankSlot = juce::jlimit (0, maxBanks - 1,
                                       activeBankSlot_.load (std::memory_order_relaxed));
    loadSoundFont (resolvedSoundFont, bankSlot);
    if (! isBankLoaded (bankSlot))
        return ApplyStatus::soundFontLoadFailed;

    switchBank (bankSlot);
    selectPreset (document.bank, document.program);

    for (const auto& stored : document.parameters)
    {
        if (stored.isChoice)
        {
            if (stored.id == ParamIDs::voiceCurve)
                applyChoice (apvts_, stored, Choices::curve);
            else if (stored.id == ParamIDs::voiceFilterRouting)
                applyChoice (apvts_, stored, Choices::filterRouting);
            else if (stored.id == ParamIDs::voiceLegato)
                applyChoice (apvts_, stored, Choices::legato);
        }
        else if (auto* parameter = apvts_.getParameter (stored.id))
        {
            parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, stored.value));
        }
    }
    return ApplyStatus::ok;
}

RomplerProcessor::ApplyStatus RomplerProcessor::applyPreset (const PresetDocument& document)
{
    return applyPreset (document, juce::File (document.soundFontPath));
}

void RomplerProcessor::requestQuickSlot (int slot) noexcept
{
    if (slot < 1 || slot > 8)
        return;
    pendingQuickSlot_.store (slot, std::memory_order_release);
    queueAsyncFlag (quickSlot);
}

void RomplerProcessor::queueAsyncFlag (unsigned int flag) noexcept
{
    pendingAsyncFlags_.fetch_or (flag, std::memory_order_release);
}

void RomplerProcessor::mirrorNormalizedParameter (const char* parameterId, float normalizedValue)
{
    if (auto* parameter = apvts_.getParameter (parameterId))
        parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalizedValue));
}

void RomplerProcessor::reloadBanksForPreparedRate()
{
    // Message thread only. prepareToPlay() publishes the rate it was called
    // with and arms reloadBanksForRate; doing the decode here keeps file I/O
    // and the resample pass off the host's prepare thread, and keeps
    // sf2Loaders_/bankNames_/retiredLoaders_ single-threaded with the editor
    // and timer paths. The flag re-arms per prepare, so a rate change that
    // lands mid-drain simply re-checks every slot on the next tick.
    const int rate = pendingBankReloadRate_.load (std::memory_order_acquire);
    const int activeSlot = activeBankSlot_.load (std::memory_order_relaxed);
    const int selectedBank = currentBank_.load (std::memory_order_relaxed);
    const int selectedProgram = currentProgram_.load (std::memory_order_relaxed);
    bool activeSlotReloaded = false;
    for (int slot = 0; slot < maxBanks; ++slot)
    {
        const auto index = static_cast<std::size_t> (slot);
        if (sf2Loaders_[index] && sf2Loaders_[index]->hostSampleRate() != rate)
        {
            loadSoundFont (juce::File (bankNames_[index]), slot);
            activeSlotReloaded |= slot == activeSlot;
        }
    }
    if (activeSlotReloaded)
    {
        // loadSoundFont() publishes the bank's first preset when it reloads
        // the active slot; keep the selection the session had before the
        // rate change.
        currentBank_.store (selectedBank, std::memory_order_relaxed);
        currentProgram_.store (selectedProgram, std::memory_order_relaxed);
    }
}

void RomplerProcessor::drainRetiredLoaders() noexcept
{
    // Message thread only. A block reader protects the raw activeLoader_ pointer
    // until processBlock exits; per-loader voice leases protect samples retained
    // across blocks. Release only loaders for which both conditions are clear.
    if (audioBlocksInFlight_.load (std::memory_order_seq_cst) != 0)
        return;

    auto loader = retiredLoaders_.begin();
    while (loader != retiredLoaders_.end())
    {
        if (! *loader || ! (*loader)->hasVoiceSampleReferences())
            loader = retiredLoaders_.erase (loader);
        else
            ++loader;
    }
}

void RomplerProcessor::timerCallback()
{
    // Drain and destroy retired SoundFont instances from prior
    // loadSoundFont() or removeBank() calls. Multiple blocks may have
    // passed since those calls, so all voices using those loaders have
    // long since retired.
    drainRetiredLoaders();

    const auto flags = pendingAsyncFlags_.exchange (0, std::memory_order_acq_rel);
    if ((flags & reloadBanksForRate) != 0)
        reloadBanksForPreparedRate();
    if ((flags & offerBundledFont) != 0)
        loadBundledSoundFont();

    if ((flags & mirrorFilterOffset) != 0)
    {
        for (int attempt = 0; attempt < maxControllerMirrorAttempts; ++attempt)
        {
            const auto generation = filterOffsetCcGeneration_.load (std::memory_order_acquire);
            if ((generation & 1u) != 0)
                continue;

            const auto capturedValue = pendingFilterOffsetNormalized_.load (std::memory_order_acquire);
            if (beforeControllerMirrorForTesting_)
                beforeControllerMirrorForTesting_();
            if (filterOffsetCcGeneration_.load (std::memory_order_acquire) != generation)
                continue;

            // CC is authoritative from audio-thread capture until this
            // deferred notification. A public APVTS CAS against host
            // automation does not exist, so this policy is intentional.
            mirrorNormalizedParameter (ParamIDs::voiceFilterOffset, capturedValue);

            if (filterOffsetCcGeneration_.load (std::memory_order_acquire) == generation)
            {
                filterOffsetMirroredGeneration_.store (generation, std::memory_order_release);
                pendingAsyncFlags_.fetch_and (~mirrorFilterOffset, std::memory_order_acq_rel);
                if (filterOffsetCcGeneration_.load (std::memory_order_acquire) != generation)
                    queueAsyncFlag (mirrorFilterOffset);
                break;
            }
        }
        if (filterOffsetCcGeneration_.load (std::memory_order_acquire)
            != filterOffsetMirroredGeneration_.load (std::memory_order_acquire))
            queueAsyncFlag (mirrorFilterOffset);
    }
    if ((flags & mirrorBusCutoff) != 0)
    {
        for (int attempt = 0; attempt < maxControllerMirrorAttempts; ++attempt)
        {
            const auto generation = busCutoffCcGeneration_.load (std::memory_order_acquire);
            if ((generation & 1u) != 0)
                continue;

            const auto capturedValue = pendingBusCutoffNormalized_.load (std::memory_order_acquire);
            if (beforeControllerMirrorForTesting_)
                beforeControllerMirrorForTesting_();
            if (busCutoffCcGeneration_.load (std::memory_order_acquire) != generation)
                continue;

            // See the matching CC71 policy above: capture is authoritative
            // until the message-thread host mirror is delivered.
            mirrorNormalizedParameter (ParamIDs::busFilterCutoff, capturedValue);

            if (busCutoffCcGeneration_.load (std::memory_order_acquire) == generation)
            {
                busCutoffMirroredGeneration_.store (generation, std::memory_order_release);
                pendingAsyncFlags_.fetch_and (~mirrorBusCutoff, std::memory_order_acq_rel);
                if (busCutoffCcGeneration_.load (std::memory_order_acquire) != generation)
                    queueAsyncFlag (mirrorBusCutoff);
                break;
            }
        }
        if (busCutoffCcGeneration_.load (std::memory_order_acquire)
            != busCutoffMirroredGeneration_.load (std::memory_order_acquire))
            queueAsyncFlag (mirrorBusCutoff);
    }
    if ((flags & mirrorLegato) != 0)
    {
        for (int attempt = 0; attempt < maxControllerMirrorAttempts; ++attempt)
        {
            const auto generation = legatoCcGeneration_.load (std::memory_order_acquire);
            if ((generation & 1u) != 0)
                continue;

            const auto capturedValue = pendingLegatoNormalized_.load (std::memory_order_acquire);
            if (beforeControllerMirrorForTesting_)
                beforeControllerMirrorForTesting_();
            if (legatoCcGeneration_.load (std::memory_order_acquire) != generation)
                continue;

            // Same CC71/74 policy: the controller is authoritative from
            // audio-thread capture until this deferred notification lands.
            mirrorNormalizedParameter (ParamIDs::voiceLegato, capturedValue);

            if (legatoCcGeneration_.load (std::memory_order_acquire) == generation)
            {
                legatoMirroredGeneration_.store (generation, std::memory_order_release);
                pendingAsyncFlags_.fetch_and (~mirrorLegato, std::memory_order_acq_rel);
                if (legatoCcGeneration_.load (std::memory_order_acquire) != generation)
                    queueAsyncFlag (mirrorLegato);
                break;
            }
        }
        if (legatoCcGeneration_.load (std::memory_order_acquire)
            != legatoMirroredGeneration_.load (std::memory_order_acquire))
            queueAsyncFlag (mirrorLegato);
    }
    if ((flags & mirrorLatency) != 0)
    {
        const auto latencySamples = pendingLatencySamples_.exchange (-1, std::memory_order_acq_rel);
        if (latencySamples >= 0)
            setLatencySamples (latencySamples);
    }
    if ((flags & quickSlot) != 0)
    {
        const auto slot = pendingQuickSlot_.exchange (0, std::memory_order_acq_rel);
        if (slot != 0 && onQuickSlotRequested)
            onQuickSlotRequested (slot);
    }
}

void RomplerProcessor::postNote (int note, bool on, int velocity)
{
    // When the queue is full the new event is dropped and processBlock() forces
    // an all-notes-off recovery at the next block boundary. This prevents a
    // lost note-off from leaving a voice stuck indefinitely.
    const UiNoteEvent event { juce::jlimit (0, 127, note),
                              juce::jlimit (0, 127, velocity),
                              on };
    if (! noteQueue_.tryPush (event))
        noteQueueOverflowed_.store (true, std::memory_order_relaxed);
}

bool RomplerProcessor::consumeUiNoteQueueOverflow() noexcept
{
    return uiNoteQueueOverflowDiagnostic_.exchange (false, std::memory_order_acq_rel);
}

void RomplerProcessor::postPitchWheel (float normalizedValue) noexcept
{
    // Message-thread UI drag; writes the same atomic that host MIDI pitch
    // wheel messages write in processBlock(), so both sources share one
    // source of truth and never fight each other (last write wins, which is
    // the same behavior a real hardware controller would have if two
    // control surfaces both touched the same wheel).
    pitchBendSemitones_.store (juce::jlimit (-1.0f, 1.0f, normalizedValue) * kPitchBendRangeSemitones,
                               std::memory_order_relaxed);
}

void RomplerProcessor::postModWheel (float normalizedValue) noexcept
{
    modWheelValue_.store (juce::jlimit (0.0f, 1.0f, normalizedValue), std::memory_order_relaxed);
}

} // namespace aod

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new aod::RomplerProcessor();
}
