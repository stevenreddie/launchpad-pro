/******************************************************************************
 
 Copyright (c) 2015, Focusrite Audio Engineering Ltd.
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 * Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 * Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 * Neither the name of Focusrite Audio Engineering Ltd., nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 *****************************************************************************/

#include <stdio.h>
#include "app.h"

#include <CoreMIDI/CoreMIDI.h>

//////////////////////////////////////////////////////////////////////////
static MIDIClientRef g_client = 0;
static MIDIPortRef g_inDevPort = 0;
static MIDIPortRef g_outDevPort = 0;
static MIDIEndpointRef g_outDevEndpoint = 0;

static const float TIMER_INTERVAL_S = 0.001; //s

// ____________________________________________________________________________
//
// Simulator "hal".  This lets you exercise your device code without having to upload
// it to the hardware, which also means you can debug it interactively.
// ____________________________________________________________________________

void hal_plot_led(u8 type, u8 index, u8 red, u8 green, u8 blue)
{
	// send this up the MIDI.  Construct sysex:
	unsigned char data[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x10, 0x0B, index, red, green, blue, 0xF7};
	
	static const int MAX_OUTPUT_BUFFER_SIZE = 256;
	
	MIDIPacketList packetLst;
	MIDIPacket *packet = MIDIPacketListInit(&packetLst);
	MIDIPacketListAdd(&packetLst, MAX_OUTPUT_BUFFER_SIZE, packet, 0, sizeof(data), data);
	
	MIDISend(g_outDevPort, g_outDevEndpoint, &packetLst);
}

void hal_send_midi(u8 port, u8 status, u8 d1, u8 d2)
{
	// TODO: send this up a virtual MIDI port?
}

void hal_send_sysex(u8 port, const u8* data, u16 length)
{
	// as above, or just dump to console?
}

//////////////////////////////////////////////////////////////////////////
static void processPacket(const unsigned char *data, int length)
{
    // parse MIDI (very naively)
    while (length > 0)
    {
        unsigned char status = data[0];
        
        // Wipe out channel (bottom four bits)
        status &= 0xF0;
        
        if (length >= 3)
        {
            switch (status)
            {
                case NOTEON:
                case NOTEOFF:
                    app_surface_event(TYPEPAD, data[1], data[2]);
                    length -= 3;
                    break;
                    
                case CC:
                    app_surface_event(TYPEPAD, data[1], data[2]);
                    length -= 3;
                    break;
                    
                case POLYAFTERTOUCH:
                    app_aftertouch_event(data[1], data[2]);
                    length -= 3;
                    break;
                    
                default:
                    break;
            }
        }
        else
        {
            // We expected at least three bytes and didn't get them, so bail
            length = 0;
        }
    }
}

static void readProc(const MIDIPacketList *pktlist, void *readProcRefCon, void *srcConnRefCon)
{
	// farm out the packets
	const MIDIPacket *packet = &pktlist->packet[0];
	for (int i=0; i <  pktlist->numPackets; ++i)
	{
		// process the packet - in our case, receive a surface message
		processPacket(packet->data, packet->length);
		
		// next
		packet = MIDIPacketNext(packet);
	}
}

static int findLaunchpadPro()
{
	// find the input hardware port
	for (ItemCount i=0; i < MIDIGetNumberOfSources(); ++i)
	{
		MIDIEndpointRef endpoint = MIDIGetSource(i);
		if (endpoint)
		{
			// get the endpoint name (always available)
			CFStringRef endpointName = NULL;
			if (noErr != MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &endpointName))
			{
				return -1;
			}
			
			if (CFStringCompare(endpointName, CFSTR("Standalone Port"), kCFCompareCaseInsensitive) == 0)
			{
				// found it!  open the endpoint.
				if (noErr != MIDIInputPortCreate(g_client, CFSTR("In"), readProc, NULL, &g_inDevPort))
				{
					return -1;
				}
				
				
				if (noErr != MIDIPortConnectSource(g_inDevPort, endpoint, NULL))
				{
					return -1;
				}
			}
		}
	}
	
	// now find the output
	for (ItemCount i=0; i < MIDIGetNumberOfDestinations(); ++i)
	{
		MIDIEndpointRef endpoint = MIDIGetDestination(i);
		if (endpoint)
		{
			// get the endpoint name (always available)
			CFStringRef endpointName = NULL;
			if (noErr != MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &endpointName))
			{
				return -1;
			}
			
			if (CFStringCompare(endpointName, CFSTR("Standalone Port"), kCFCompareCaseInsensitive) == 0)
			{
				// found it!  open the endpoint.
				if (noErr != MIDIOutputPortCreate(g_client, CFSTR("Out"), &g_outDevPort))
				{
					return -1;
				}
				g_outDevEndpoint = endpoint;
			}
		}
	}
	
	return !(g_inDevPort && g_outDevPort);
}

// ____________________________________________________________________________

static void timerCallback(CFRunLoopTimerRef timer, void * info)
{
    app_timer_event();
    CFRunLoopSourceSignal(info);
}

// ____________________________________________________________________________

int main(int argc, char * argv[])
{
	// open MIDI ports and wire them up
	CFStringRef strName = CFStringCreateWithCString(NULL, "Launchpad Pro Simulator", kCFStringEncodingASCII);
	if (noErr == MIDIClientCreate(strName, NULL, NULL, &g_client))
	{
		CFRelease(strName);
	}
	else
	{
		// failed to open MIDI client
		return -1;
	}
	
	if (findLaunchpadPro())
	{
		// no Launchpad Pro connected
		return -2;
	}

	// now start things up
	app_init();
	
    // start a timer loop
    CFRunLoopSourceContext source_context;
    bzero(&source_context, sizeof(source_context));
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, & source_context);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    
    CFRunLoopTimerContext timer_context;
    bzero(&timer_context, sizeof(timer_context));
    timer_context.info = source;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(), TIMER_INTERVAL_S, 0, 0, timerCallback, &timer_context);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    
    CFRunLoopRun();
    
    return EXIT_SUCCESS;
}