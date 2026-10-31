/**
 *
 * Copyright (c) 2013-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include <stdarg.h>
#include <bitset>

#include "PluginProcessor.h"
#include "ui/PluginEditor.h"
#include "parameter/Model.h"

#include "debugger/Logger.h"
#include "msfa/synth.h"

#include "midi/MidiCCHandler.h"
#include "debugger/MemoryLogger.h"

#if JUCE_MSVC
    #pragma comment (lib, "kernel32.lib")
    #pragma comment (lib, "user32.lib")
    #pragma comment (lib, "wininet.lib")
    #pragma comment (lib, "advapi32.lib")
    #pragma comment (lib, "ws2_32.lib")
    #pragma comment (lib, "version.lib")
    #pragma comment (lib, "shlwapi.lib")
    #pragma comment (lib, "winmm.lib")
	#pragma comment (lib, "DbgHelp.lib")
	#pragma comment (lib, "Imm32.lib")

	#ifdef _NATIVE_WCHAR_T_DEFINED
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppwd.lib")
		#else
			#pragma comment (lib, "comsuppw.lib")
		#endif
	#else
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppd.lib")
		#else
			#pragma comment (lib, "comsupp.lib")
		#endif
	#endif

#endif

//==============================================================================
DexedAudioProcessor::DexedAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("output", AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr) {

    MemoryLogger::getInstance()->install();

    DXLOG("     _                   _ ");
    DXLOG("  __| | _____  _____  __| |");
    DXLOG(" / _` |/ _ \\ \\/ / _ \\/ _` |");
    DXLOG("| (_| |  __/>  <  __/ (_| |");
    DXLOG(" \\__,_|\\___/_/\\_\\___|\\__,_|");
    DXLOG("VERSION " DEXED_VERSION);

    mapParameters();

    synthTuningState = createStandardTuning();
    synthTuningStateLast = createStandardTuning();
    engine.setTuning(synthTuningState);
    engine.setPatch(activeProgram.getRawData());

    midiCCMapper = std::make_unique<MidiCCHandler>(parameters);

    vuSignal = 0;

    resolvAppDir();
    
    sendSysexChange = true;
    normalizeDxVelocity = false;
    sysexComm.listener = this;
    sysexComm.setCommandFifo(&command);
    showKeyboard = true;
    
    memset(&voiceStatus, 0, sizeof(VoiceStatus));
    // the engine starts with Mark I, pitch bend range 3/3, no pitch step and no master tune
    
    loadPreference();

    setupStartupCart();
    setCurrentProgram(0);

    mtsClient = NULL;
    mtsClient = MTS_RegisterClient();
    engine.setMtsClient(mtsClient);
}

DexedAudioProcessor::~DexedAudioProcessor() {
    TRACE("Bye");
    if (mtsClient) MTS_DeregisterClient(mtsClient);
}

//==============================================================================
void DexedAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    engine.prepare(sampleRate);

    vuDecayFactor = VuMeterOutput::getDecayFactor(sampleRate);

    keyboardState.reset();

    command.reset();
}

void DexedAudioProcessor::releaseResources() {
    engine.release();

    keyboardState.reset();
}

void DexedAudioProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    int numSamples = buffer.getNumSamples();
    int i;

    command.call(*this);

    if ( refreshVoice.compareAndSetBool(false, true) ) {
        engine.refreshVoices();
    }

    keyboardState.processNextMidiBuffer(midiMessages, 0, numSamples, true);

    juce::MidiBufferIterator it = midiMessages.begin(), end = midiMessages.end();
    float *channelData = buffer.getWritePointer(0);

    MidiEvents events(*this, it, end);
    engine.render(channelData, numSamples, events);

    for(i=0; i<numSamples; i++) {
        float s = std::abs(channelData[i]);

        if (s > vuSignal)
            vuSignal = s;
        else if (vuSignal > /*0.001f*/ 1.26E-4F) // 1.26E-4 is equivalent to -39 dB, the min amplitude associated to leftmost LED
            vuSignal *= vuDecayFactor;
        else
            vuSignal = 0;
    }
    
    // DX7 is a mono synth, but copy it to the right channel is available
    if ( buffer.getNumChannels() > 1 )
        buffer.copyFrom(1, 0, channelData, numSamples, 1);

    midiMessages.clear();
    for (const auto &msg : outputMidiMessageQueue) {
        midiMessages.addEvent(msg, 0);
    }
    outputMidiMessageQueue.clear();
}

void DexedAudioProcessor::queueOutputMidiMessage(const MidiMessage &message) {
    outputMidiMessageQueue.push_back(message);
}

//==============================================================================
// This creates new instances of the plugin..
AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new DexedAudioProcessor();
}

void DexedAudioProcessor::panic() {
    engine.panic();
    keyboardState.reset();
}

int DexedAudioProcessor::getEngineType() {
    return engine.getEngineType();
}

void DexedAudioProcessor::setEngineType(int tp) {
    TRACE("settings engine %d", tp);
    engine.setEngineType(tp);
}

void DexedAudioProcessor::resetMonoMode() {
    panic();
    engine.setMonoMode(parameters.getParameter(IDs::monoMode.name)->getValue() > 0.5f);
}

// ====================================================================
bool DexedAudioProcessor::peekVoiceStatus() {
    return engine.peekVoiceStatus(voiceStatus);
}

bool DexedAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    return layouts.getMainOutputChannelSet() == AudioChannelSet::mono()
                || layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

bool DexedAudioProcessor::acceptsMidi() const {
    return true;
}

bool DexedAudioProcessor::producesMidi() const {
    return true;
}

double DexedAudioProcessor::getTailLengthSeconds() const {
    return 0.0;
}

const String DexedAudioProcessor::getName() const {
    return JucePlugin_Name;
}

//==============================================================================
bool DexedAudioProcessor::hasEditor() const {
    return true; // (change this to false if you choose to not supply an editor)
}

AudioProcessorEditor* DexedAudioProcessor::createEditor() {
    AudioProcessorEditor* editor = new DexedAudioProcessorEditor (this);
    return editor;
}

void DexedAudioProcessor::setZoomFactor(float factor) {
    zoomFactor = factor;
}

void DexedAudioProcessor::applyProgram(const Program &program) {
    panic();
    activeProgram = program;
    activeProgram.pushToParameters(parameters);
    engine.resetLfo();
    refreshVoice.set(true);
    programChanged.set(true);
}

void dexed_trace(const char *source, const char *fmt, ...) {
    char output[4096];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(output, 4095, fmt, argptr);
    va_end(argptr);

    String dest;
    dest << source << " " << output;
    Logger::writeToLog(dest);
}
