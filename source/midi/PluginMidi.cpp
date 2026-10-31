#include "PluginProcessor.h"
#include "MidiCCHandler.h"

void DexedAudioProcessor::processMidiMessages(MidiBufferIterator &cur, MidiBufferIterator &end, int samplePos) {
    for(;cur != end;++cur) {
        juce::MidiMessageMetadata meta = *cur;
        if ( meta.samplePosition > samplePos )
            return;
        const MidiMessage message = meta.getMessage();
        processMidiMessage(&message);
    }
}

void DexedAudioProcessor::processMidiMessage(const MidiMessage *msg) {
    if ( msg->isSysEx() ) {
        handleIncomingMidiMessage(NULL, *msg);
        return;
    }

    const uint8 *buf  = msg->getRawData();

    switch ( engine.processMidiMessage(buf, msg->getChannel()) ) {
        case SynthEngine::MidiResult::ProgramChange:
            setCurrentProgram(buf[1]);
            break;
        case SynthEngine::MidiResult::UnhandledControlChange:
            //TRACE("handle channel %d CC %d = %d", channel, ctrl, value);
            midiCCMapper->processControlChange(buf[1], buf[2]);
            break;
        case SynthEngine::MidiResult::Panic:
            keyboardState.reset();
            break;
        case SynthEngine::MidiResult::Handled:
            break;
    }
}

void DexedAudioProcessor::handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) {
    if ( message.isActiveSense() )
        return;

#ifdef IMPLEMENT_MidiMonitor
    sysexComm.inActivity = true; // indicate to MidiMonitor that a MIDI messages (other than Active Sense) is received
#endif //IMPLEMENT_MidiMonitor

    const uint8 *buf = message.getRawData();
    int sz = message.getRawDataSize();

    //TRACE("%X %X %X %X %X %X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    if ( ! message.isSysEx() )
        return;

    // test if it is a Yamaha Sysex
    if ( buf[1] != 0x43 ) {
        DXLOG("Sysex message is not a yamaha sysex %d", buf[1]);
        return;
    }

    int substatus = buf[2] >> 4;
    switch(substatus) {
        case 0 : {
            // single voice dump
            if ( buf[3] == 0 ) {
                if ( sz < 156 ) {
                    DXLOG("wrong single voice datasize %d", sz);
                    return;
                }
                if ( updateProgramFromSysex(buf+6) )
                    DXLOG("bad checksum when updating program from sysex message");
            }

            // 32 voice dump
            if ( buf[3] == 9 ) {
                if ( sz < 4104 ) {
                    DXLOG("wrong 32 voice dump data size %d", sz);
                    return;
                }

                Cartridge received;
                if ( received.load(buf, sz) == 0 ) {
                    loadCartridge(received);
                    setCurrentProgram(0);
                }
            }
        }
        break;
        case 1 : {
            // parameter change
            if ( sz < 7 ) {
            DXLOG("wrong single voice parameter datasize %d", sz);
            return;
            }

            uint8 offset = (buf[3] << 7) + buf[4];
            uint8 value = buf[5];

            DXLOG("Parameter change message offset:%d value:%d", offset, value);

            if ( offset > 155 ) {
                DXLOG("parameter wrong offset size");
                return;
            }

            setDxParameter(offset, value);
        }
        break;
        case 2: {
            if ( buf[3] == 0 ) {
                // single voice request
                sendCurrentSysexProgram();
            } else if ( buf[3] == 9 ) {
                // cart request
                sendCurrentSysexCartridge();
            } else {
                TRACE("Unknown voice request: %d", buf[3]);
            }
        }
        return;

        default: {
            TRACE("unknown sysex substatus: %d", substatus);
        }
        return;
    }

    // Voice/cartridge dumps refresh the UI via applyProgram/loadCartridge
    // (programChanged), and parameter changes via setDxParameter -> the bound
    // control follows automatically. Nothing else to refresh here.
}
