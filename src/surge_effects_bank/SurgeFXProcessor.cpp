/*
  ==============================================================================

    This file was auto-generated!

    It contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "SurgeFXProcessor.h"
#include "SurgeFXEditor.h"
#include "DebugHelpers.h"

//==============================================================================
SurgefxAudioProcessor::SurgefxAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true))
{
    effectNum = fxt_delay;
    storage.reset(new SurgeStorage());
    storage->userPrefOverrides[Surge::Storage::HighPrecisionReadouts] = std::make_pair(0, "");

    fxstorage = &(storage->getPatch().fx[0]);
    audio_thread_surge_effect.reset();
    resetFxType(effectNum, false);
    fxstorage->return_level.id = -1;
    setupStorageRanges((Parameter *)fxstorage, &(fxstorage->p[n_fx_params - 1]));

    for (int i = 0; i < n_fx_params; ++i)
    {
        char lb[256], nm[256];
        snprintf(lb, 256, "fx_parm_%d", i);
        snprintf(nm, 256, "FX Parameter %d", i);

        addParameter(fxParams[i] = new juce::AudioParameterFloat(
                         lb, nm, juce::NormalisableRange<float>(0.0, 1.0),
                         fxstorage->p[fx_param_remap[i]].get_value_f01()));
        fxBaseParams[i] = fxParams[i];
    }
    addParameter(fxType = new juce::AudioParameterInt("fxtype", "FX Type", fxt_delay,
                                                      n_fx_types - 1, effectNum));
    fxBaseParams[n_fx_params] = fxType;

    for (int i = 0; i < n_fx_params; ++i)
    {
        char lb[256], nm[256];
        snprintf(lb, 256, "fx_temposync_%d", i);
        snprintf(nm, 256, "FX Temposync %d", i);

        // if you change this 0xFF also change the divide in the setValueNotifyingHost in
        // setFXParamExtended etc
        addParameter(fxParamFeatures[i] = new juce::AudioParameterInt(lb, nm, 0, 0xFF, 0));
        *(fxParamFeatures[i]) = paramFeatureFromParam(&(fxstorage->p[fx_param_remap[i]]));
        fxBaseParams[i + n_fx_params + 1] = fxParamFeatures[i];
    }

    for (int i = 0; i < 2 * n_fx_params + 1; ++i)
    {
        fxBaseParams[i]->addListener(this);
        changedParams[i] = false;
        isUserEditing[i] = false;
    }

    for (int i = 0; i < n_fx_params; ++i)
    {
        wasParamFeatureChanged[i] = false;
    }

    paramChangeListener = []() {};
    resettingFx = false;
}

SurgefxAudioProcessor::~SurgefxAudioProcessor() {}

//==============================================================================
const juce::String SurgefxAudioProcessor::getName() const { return JucePlugin_Name; }

bool SurgefxAudioProcessor::acceptsMidi() const { return false; }

bool SurgefxAudioProcessor::producesMidi() const { return false; }

bool SurgefxAudioProcessor::isMidiEffect() const { return false; }

double SurgefxAudioProcessor::getTailLengthSeconds() const { return 2.0; }

int SurgefxAudioProcessor::getNumPrograms()
{
    return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
              // so this should be at least 1, even if you're not really implementing programs.
}

int SurgefxAudioProcessor::getCurrentProgram() { return 0; }

void SurgefxAudioProcessor::setCurrentProgram(int index) {}

const juce::String SurgefxAudioProcessor::getProgramName(int index) { return "Default"; }

void SurgefxAudioProcessor::changeProgramName(int index, const juce::String &newName) {}

//==============================================================================
void SurgefxAudioProcessor::prepareToPlay(double sr, int samplesPerBlock)
{
    storage->setSamplerate(sr);
}

void SurgefxAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool SurgefxAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
    // the sidechain can take any layout, the main bus needs to be the same on the input and output
    return layouts.getMainInputChannelSet() == layouts.getMainOutputChannelSet() &&
           !layouts.getMainInputChannelSet().isDisabled() &&
           layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

#define is_aligned(POINTER, BYTE_COUNT) (((uintptr_t)(const void *)(POINTER)) % (BYTE_COUNT) == 0)

void SurgefxAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                         juce::MidiBuffer &midiMessages)
{
    if (resettingFx || !surge_effect)
        return;

    juce::ScopedNoDenormals noDenormals;

    if (surge_effect->checkHasInvalidatedUI())
    {
        resetFxParams(true);
    }

    float thisBPM = 120.0;
    auto playhead = getPlayHead();
    if (playhead)
    {
        juce::AudioPlayHead::CurrentPositionInfo cp;
        playhead->getCurrentPosition(cp);
        thisBPM = cp.bpm;
    }

    if (storage && thisBPM != lastBPM)
    {
        lastBPM = thisBPM;
        storage->temposyncratio = thisBPM / 120.0;
        storage->temposyncratio_inv = 1.f / storage->temposyncratio;
    }

    auto mainInputOutput = getBusBuffer(buffer, true, 0);
    auto sideChainInput = getBusBuffer(buffer, true, 1);

    // FIXME: Check: has type changed?
    int pt = *fxType;

    if (effectNum != pt)
    {
        effectNum = pt;
        resetFxType(effectNum);
    }

    if (audio_thread_surge_effect.get() != surge_effect.get())
    {
        audio_thread_surge_effect = surge_effect;
    }

    for (int outPos = 0; outPos < buffer.getNumSamples() && !resettingFx; outPos += BLOCK_SIZE)
    {
        auto outL = mainInputOutput.getWritePointer(0, outPos);
        auto outR = mainInputOutput.getWritePointer(1, outPos);

        auto sideChainBus = getBus(true, 1);
        if (effectNum == fxt_vocoder && sideChainBus && sideChainBus->isEnabled())
        {
            auto sideL = sideChainInput.getReadPointer(0, outPos);
            auto sideR = sideChainInput.getReadPointer(1, outPos);

            memcpy(storage->audio_in_nonOS[0], sideL, BLOCK_SIZE * sizeof(float));
            memcpy(storage->audio_in_nonOS[1], sideR, BLOCK_SIZE * sizeof(float));
        }

        for (int i = 0; i < n_fx_params; ++i)
        {
            fxstorage->p[fx_param_remap[i]].set_value_f01(*fxParams[i]);
            paramFeatureOntoParam(&(fxstorage->p[fx_param_remap[i]]), *(fxParamFeatures[i]));
        }
        copyGlobaldataSubset(storage_id_start, storage_id_end);

        if (is_aligned(outL, 16) && is_aligned(outR, 16))
        {
            audio_thread_surge_effect->process(outL, outR);
        }
        else
        {
            float bufferL alignas(16)[BLOCK_SIZE], bufferR alignas(16)[BLOCK_SIZE];

            auto inL = mainInputOutput.getReadPointer(0, outPos);
            auto inR = mainInputOutput.getReadPointer(1, outPos);

            memcpy(bufferL, inL, BLOCK_SIZE * sizeof(float));
            memcpy(bufferR, inR, BLOCK_SIZE * sizeof(float));

            audio_thread_surge_effect->process(bufferL, bufferR);

            memcpy(outL, bufferL, BLOCK_SIZE * sizeof(float));
            memcpy(outR, bufferR, BLOCK_SIZE * sizeof(float));
        }
    }
}

//==============================================================================
bool SurgefxAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor *SurgefxAudioProcessor::createEditor()
{
    return new SurgefxAudioProcessorEditor(*this);
}

//==============================================================================
void SurgefxAudioProcessor::getStateInformation(juce::MemoryBlock &destData)
{
    std::unique_ptr<juce::XmlElement> xml(new juce::XmlElement("surgefx"));
    xml->setAttribute("streamingVersion", (int)1);
    for (int i = 0; i < n_fx_params; ++i)
    {
        char nm[256];
        snprintf(nm, 256, "fxp_%d", i);
        float val = *(fxParams[i]);
        xml->setAttribute(nm, val);

        snprintf(nm, 256, "fxp_param_features_%d", i);
        int pf = *(fxParamFeatures[i]);
        xml->setAttribute(nm, pf);
    }
    xml->setAttribute("fxt", effectNum);

    copyXmlToBinary(*xml, destData);
}

void SurgefxAudioProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState.get() != nullptr)
    {
        if (xmlState->hasTagName("surgefx"))
        {
            effectNum = xmlState->getIntAttribute("fxt", fxt_delay);
            resetFxType(effectNum, false);

            for (int i = 0; i < n_fx_params; ++i)
            {
                char nm[256];
                snprintf(nm, 256, "fxp_%d", i);
                float v = xmlState->getDoubleAttribute(nm, 0.0);
                fxstorage->p[fx_param_remap[i]].set_value_f01(v);

                // Legacy unstream
                snprintf(nm, 256, "fxp_temposync_%d", i);
                if (xmlState->hasAttribute(nm))
                {
                    bool b = xmlState->getBoolAttribute(nm, false);
                    fxstorage->p[fx_param_remap[i]].temposync = b;
                }

                // Modern unstream
                snprintf(nm, 256, "fxp_param_features_%d", i);
                if (xmlState->hasAttribute(nm))
                {
                    int pf = xmlState->getIntAttribute(nm, 0);
                    paramFeatureOntoParam(&(fxstorage->p[fx_param_remap[i]]), pf);
                }
            }
            updateJuceParamsFromStorage();
        }
    }
}

void SurgefxAudioProcessor::reorderSurgeParams()
{
    if (surge_effect.get())
    {
        for (auto i = 0; i < n_fx_params; ++i)
            fx_param_remap[i] = i;

        std::vector<std::pair<int, int>> orderTrack;
        for (auto i = 0; i < n_fx_params; ++i)
        {
            if (fxstorage->p[i].posy_offset && fxstorage->p[i].ctrltype != ct_none)
            {
                orderTrack.push_back(std::pair<int, int>(i, i * 2 + fxstorage->p[i].posy_offset));
            }
            else
            {
                orderTrack.push_back(std::pair<int, int>(i, 10000));
            }
        }
        std::sort(orderTrack.begin(), orderTrack.end(),
                  [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
                      return a.second < b.second;
                  });

        int idx = 0;
        for (auto a : orderTrack)
        {
            fx_param_remap[idx++] = a.first;
        }
    }

    // I hate having to use this API so much...
    for (auto i = 0; i < n_fx_params; ++i)
    {
        if (fxstorage->p[fx_param_remap[i]].ctrltype == ct_none)
        {
            group_names[i] = "-";
        }
        else
        {
            int fpos = i + fxstorage->p[fx_param_remap[i]].posy / 10 +
                       fxstorage->p[fx_param_remap[i]].posy_offset;
            for (auto j = 0; j < n_fx_params; ++j)
            {
                if (surge_effect->group_label(j) &&
                    surge_effect->group_label_ypos(j) <= fpos // constants for SurgeGUIEditor. Sigh.
                )
                {
                    group_names[i] = surge_effect->group_label(j);
                }
            }
        }
    }
}

void SurgefxAudioProcessor::resetFxType(int type, bool updateJuceParams)
{
    resettingFx = true;
    effectNum = type;
    fxstorage->type.val.i = effectNum;

    for (int i = 0; i < n_fx_params; ++i)
        fxstorage->p[i].set_type(ct_none);

    surge_effect.reset(spawn_effect(effectNum, storage.get(), &(storage->getPatch().fx[0]),
                                    storage->getPatch().globaldata));
    if (surge_effect)
    {
        surge_effect->init();
        surge_effect->init_ctrltypes();
        surge_effect->init_default_values();
    }
    resetFxParams(updateJuceParams);
}

void SurgefxAudioProcessor::resetFxParams(bool updateJuceParams)
{
    reorderSurgeParams();

    /*
    ** TempoSync etc settings may linger so whack them all to false again
    */
    for (int i = 0; i < n_fx_params; ++i)
        paramFeatureOntoParam(&(fxstorage->p[i]), 0);

    if (updateJuceParams)
    {
        updateJuceParamsFromStorage();
    }

    resettingFx = false;
}

void SurgefxAudioProcessor::updateJuceParamsFromStorage()
{
    SupressGuard sg(&supressParameterUpdates);
    for (int i = 0; i < n_fx_params; ++i)
    {
        *(fxParams[i]) = fxstorage->p[fx_param_remap[i]].get_value_f01();
        int32_t switchVal = paramFeatureFromParam(&(fxstorage->p[fx_param_remap[i]]));
        *(fxParamFeatures[i]) = switchVal;
    }
    *(fxType) = effectNum;

    for (int i = 0; i < n_fx_params; ++i)
    {
        changedParamsValue[i] = fxstorage->p[fx_param_remap[i]].get_value_f01();
        changedParams[i] = true;
    }
    changedParamsValue[n_fx_params] = effectNum;
    changedParams[n_fx_params] = true;

    triggerAsyncUpdate();
}

void SurgefxAudioProcessor::copyGlobaldataSubset(int start, int end)
{
    for (int i = start; i < end; ++i)
    {
        storage->getPatch().globaldata[i].i = storage->getPatch().param_ptr[i]->val.i;
    }
}

void SurgefxAudioProcessor::setupStorageRanges(Parameter *start, Parameter *endIncluding)
{
    int min_id = 100000, max_id = -1;
    Parameter *oap = start;
    while (oap <= endIncluding)
    {
        if (oap->id >= 0)
        {
            if (oap->id > max_id)
                max_id = oap->id;
            if (oap->id < min_id)
                min_id = oap->id;
        }
        oap++;
    }

    storage_id_start = min_id;
    storage_id_end = max_id + 1;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new SurgefxAudioProcessor(); }
