/*
 * CMF2IMF - convert CMF files into id Software IMF files
 * Copyright (C) 2005-2010 Adam Nielsen <malvineous@shikadi.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Bugs/limitations in this version:
 *
 *  - Percussion is not converted.  This can be worked around by placing
 *    each percussion instrument on its own channel before conversion.
 */

#include <camoto/iostream_helpers.hpp>
#include "cmf.hpp"

namespace cmf {

using namespace camoto;

// ------------------------------
// OPTIONS
// ------------------------------

// The official Creative Labs CMF player seems to ignore the note velocity
// (playing every note at the same volume), but you can uncomment this to
// allow the note velocity to affect the volume (as presumably the composer
// originally intended.)
//
//#define USE_VELOCITY
//
// The Xargon demo song is a good example of a song that uses note velocity.

//
// --- Code begins ---
//

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

// OPL register offsets
#define BASE_CHAR_MULT  0x20
#define BASE_SCAL_LEVL  0x40
#define BASE_ATCK_DCAY  0x60
#define BASE_SUST_RLSE  0x80
#define BASE_FNUM_L     0xA0
#define BASE_KEYON_FREQ 0xB0
#define BASE_RHYTHM     0xBD
#define BASE_WAVE       0xE0
#define BASE_FEED_CONN  0xC0

#define OPLBIT_KEYON    0x20 // Bit in BASE_KEYON_FREQ register for turning a note on

// Supplied with a channel, return the offset from a base OPL register for the
// Modulator cell (e.g. channel 4's modulator is at offset 0x09.  Since 0x60 is
// the attack/decay function, register 0x69 will thus set the attack/decay for
// channel 4's modulator.)  (channels go from 0 to 8 inclusive)
#define OPLOFFSET(channel)   (((channel) / 3) * 8 + ((channel) % 3))

// These 16 instruments are repeated to fill up the 128 available slots.  A CMF
// file can override none/some/all of the 128 slots with custom instruments,
// so any that aren't overridden are still available for use with these default
// patches.  The Word Rescue CMFs are good examples of songs that rely on these
// default patches.
char cDefaultPatches[] =
"\x01\x11\x4F\x00\xF1\xD2\x53\x74\x00\x00\x06"
"\x07\x12\x4F\x00\xF2\xF2\x60\x72\x00\x00\x08"
"\x31\xA1\x1C\x80\x51\x54\x03\x67\x00\x00\x0E"
"\x31\xA1\x1C\x80\x41\x92\x0B\x3B\x00\x00\x0E"
"\x31\x16\x87\x80\xA1\x7D\x11\x43\x00\x00\x08"
"\x30\xB1\xC8\x80\xD5\x61\x19\x1B\x00\x00\x0C"
"\xF1\x21\x01\x00\x97\xF1\x17\x18\x00\x00\x08"
"\x32\x16\x87\x80\xA1\x7D\x10\x33\x00\x00\x08"
"\x01\x12\x4F\x00\x71\x52\x53\x7C\x00\x00\x0A"
"\x02\x03\x8D\x00\xD7\xF5\x37\x18\x00\x00\x04"
"\x21\x21\xD1\x00\xA3\xA4\x46\x25\x00\x00\x0A"
"\x22\x22\x0F\x00\xF6\xF6\x95\x36\x00\x00\x0A"
"\xE1\xE1\x00\x00\x44\x54\x24\x34\x02\x02\x07"
"\xA5\xB1\xD2\x80\x81\xF1\x03\x05\x00\x00\x02"
"\x71\x22\xC5\x00\x6E\x8B\x17\x0E\x00\x00\x02"
"\x32\x21\x16\x80\x73\x75\x24\x57\x00\x00\x0E";

player::player(std::istream& data, FN_SETREGISTER cbSetRegister, FN_DELAY cbDelay)
	throw (std::ios::failure) :
	data(data),
	cbSetRegister(cbSetRegister),
	cbDelay(cbDelay),
	pInstruments(NULL),
	bPercussive(false),
	iTranspose(0),
	iPrevCommand(0),
	iNoteCount(0)
{
	assert(OPLOFFSET(1-1) == 0x00);
	assert(OPLOFFSET(5-1) == 0x09);
	assert(OPLOFFSET(9-1) == 0x12);

	for (int i = 0; i < 9; i++) {
		this->chOPL[i].iNoteStart = 0; // no note playing atm
		this->chOPL[i].iMIDINote = 0;
		this->chOPL[i].iMIDIChannel = 0;
		this->chOPL[i].iMIDIPatch = -1;

		this->chMIDI[i].iPatch = 0;
		this->chMIDI[i].iPitchbend = 8192;
	}
	for (int i = 9; i < 16; i++) {
		this->chMIDI[i].iPatch = 0;
		this->chMIDI[i].iPitchbend = 8192;
	}

	memset(this->iCurrentRegs, 0, 256);

	std::string sig;
	this->data >> fixedLength(sig, 4);
	if (sig.compare("CTMF")) {
		throw std::ios::failure("Input file is not a CMF file! (CTMF header missing)");
	}
	uint16_t iVer;
	this->data >> u16le(iVer);
	if ((iVer != 0x0101) && (iVer != 0x0100)) {
		throw std::ios::failure("CMF file is not v1.0 or v1.1");
	}

	this->data
		>> u16le(this->cmfHeader.iInstrumentBlockOffset)
		>> u16le(this->cmfHeader.iMusicOffset)
		>> u16le(this->cmfHeader.iTicksPerQuarterNote)
		>> u16le(this->cmfHeader.iTicksPerSecond)
		>> u16le(this->cmfHeader.iTagOffsetTitle)
		>> u16le(this->cmfHeader.iTagOffsetComposer)
		>> u16le(this->cmfHeader.iTagOffsetRemarks)
	;
	this->data.read((char *)this->cmfHeader.iChannelsInUse, 16);
	switch (iVer) {
		case 0x0100: {
			uint8_t temp;
			this->data
				>> u8(temp);
			;
			this->cmfHeader.iNumInstruments = temp;
			break;
		}
		case 0x0101:
			this->data
				>> u16le(this->cmfHeader.iNumInstruments)
				>> u16le(this->cmfHeader.iTempo)
			;
			break;
	}
}

player::~player()
	throw ()
{
	if (this->pInstruments) delete[] this->pInstruments;
}

void player::init(void)
	throw (std::ios::failure)
{
	this->data.seekg(this->cmfHeader.iInstrumentBlockOffset);

	this->pInstruments = new SBI[128];

	for (int i = 0; i < this->cmfHeader.iNumInstruments; i++) {
		this->data
			>> u8(this->pInstruments[i].op[0].iCharMult)
			>> u8(this->pInstruments[i].op[1].iCharMult)
			>> u8(this->pInstruments[i].op[0].iScalingOutput)
			>> u8(this->pInstruments[i].op[1].iScalingOutput)
			>> u8(this->pInstruments[i].op[0].iAttackDecay)
			>> u8(this->pInstruments[i].op[1].iAttackDecay)
			>> u8(this->pInstruments[i].op[0].iSustainRelease)
			>> u8(this->pInstruments[i].op[1].iSustainRelease)
			>> u8(this->pInstruments[i].op[0].iWaveSel)
			>> u8(this->pInstruments[i].op[1].iWaveSel)
			>> u8(this->pInstruments[i].iConnection)
		;
		this->data.seekg(5, std::ios::cur); // skip over the padding bytes
	}

	// Set the rest of the instruments to the CMF defaults
	for (int i = this->cmfHeader.iNumInstruments; i < 128; i++) {
		this->pInstruments[i].op[0].iCharMult =       cDefaultPatches[(i % 16) * 11 + 0];
		this->pInstruments[i].op[1].iCharMult =       cDefaultPatches[(i % 16) * 11 + 1];
		this->pInstruments[i].op[0].iScalingOutput =  cDefaultPatches[(i % 16) * 11 + 2];
		this->pInstruments[i].op[1].iScalingOutput =  cDefaultPatches[(i % 16) * 11 + 3];
		this->pInstruments[i].op[0].iAttackDecay =    cDefaultPatches[(i % 16) * 11 + 4];
		this->pInstruments[i].op[1].iAttackDecay =    cDefaultPatches[(i % 16) * 11 + 5];
		this->pInstruments[i].op[0].iSustainRelease = cDefaultPatches[(i % 16) * 11 + 6];
		this->pInstruments[i].op[1].iSustainRelease = cDefaultPatches[(i % 16) * 11 + 7];
		this->pInstruments[i].op[0].iWaveSel =        cDefaultPatches[(i % 16) * 11 + 8];
		this->pInstruments[i].op[1].iWaveSel =        cDefaultPatches[(i % 16) * 11 + 9];
		this->pInstruments[i].iConnection =           cDefaultPatches[(i % 16) * 11 + 10];
	}

	std::cout << "Found " << this->cmfHeader.iNumInstruments << " instrument definitions" << std::endl;

	// Testing.  Set the last five instruments to the percussive ones.
	this->bPercussive = true;
//	this->pInstruments[6].op[0].iScalingOutput = 0x4F;
	for (int i = this->cmfHeader.iNumInstruments - 5, j = 11; j < 16; i++, j++) {
		this->chMIDI[j].iPatch = i;
		std::cout << "Presetting MIDI channel " << j << " to patch " << i << std::endl;
		uint8_t iPercChannel = getPercChannel(j);
		this->MIDIchangeInstrument(iPercChannel, j, i);
	}
	this->bPercussive = false;

	this->data.seekg(this->cmfHeader.iMusicOffset, std::ios::beg);

	// Initialise
	// Enable use of WaveSel register on OPL3 (even though we're only an OPL2!)
	this->setReg(0x01, 0x20);

	// Really make sure CSM+SEL are off (again, Creative's player...)
	this->setReg(0x08, 0x00);

/*
	this->setReg(0x08, 0x04); // Creative's player does this - not sure why though...
	this->setReg(0x08, 0x0B);
	this->setReg(0x08, 0x0D);
	this->setReg(0x08, 0x0F);
	this->setReg(0x08, 0x16);
	this->setReg(0x08, 0x18);
	this->setReg(0x08, 0x1A);
*/

	// Set a default frequency for the cymbal and hihat (apparently this can't be changed by a song, even though it
	// needs to be changed sometimes to sound right!)  Some songs don't get an initial value, which is why we need to
	// here.  (Otherwise the beginning of a song can sound different each time it's played!) - e.g. kiloblaster/song_4.cmf
/*
//	this->setReg(BASE_FNUM_L + 6, 432 & 0xFF);
//	this->setReg(BASE_KEYON_FREQ + 6, (2 << 2) | (432 >> 8));
	this->setReg(BASE_FNUM_L + 7, 458 & 0xFF);
	this->setReg(BASE_KEYON_FREQ + 7, (2 << 2) | (458 >> 8));

	this->setReg(BASE_FNUM_L + 8, 1);
	this->setReg(BASE_KEYON_FREQ + 8, (4 << 2) | 1);
	// */

	// This freq setting is required for the hihat to sound correct at the start
	// of funky.cmf, even though it's for an unrelated channel.
	// If it's here however, it makes the hihat in Word Rescue's theme.cmf
	// sound really bad.
	// TODO: How do we figure out whether we need it or not???
	this->setReg(BASE_FNUM_L + 8, 514 & 0xFF);
	this->setReg(BASE_KEYON_FREQ + 8, (1 << 2) | (514 >> 8));

	// default freqs?
	//this->setReg(BASE_FNUM_L + 7, 343 & 0xFF);
	//this->setReg(BASE_KEYON_FREQ + 7, (3 << 2) | (343 >> 8));
	this->setReg(BASE_FNUM_L + 7, 509 & 0xFF);
	this->setReg(BASE_KEYON_FREQ + 7, (2 << 2) | (509 >> 8));
	this->setReg(BASE_FNUM_L + 6, 432 & 0xFF);
	this->setReg(BASE_KEYON_FREQ + 6, (2 << 2) | (432 >> 8));

	// Can't set the Top Cymbal/Tom Tom pitch here (channel 8-1) because otherwise
	// it influences the Hihat pitch! (channel 9-1)

/*	this->setReg(BASE_FNUM_L + 0, 0x22);
	this->setReg(BASE_FNUM_L + 1, 0x22);
	this->setReg(BASE_FNUM_L + 2, 0x00);
	this->setReg(BASE_FNUM_L + 3, 0x6C);
	this->setReg(BASE_FNUM_L + 4, 0x98);
	this->setReg(BASE_FNUM_L + 5, 0x8A);
	this->setReg(BASE_FNUM_L + 6, 0xE6);
	this->setReg(BASE_FNUM_L + 7, 0x03);
	this->setReg(BASE_FNUM_L + 8, 0x57);*/

	// Amplify AM + VIB depth.  Creative's CMF player does this, and there
	// doesn't seem to be any way to stop it from doing so - except for the
	// non-standard controller 0x63 I added :-)
	this->setReg(0xBD, 0xC0);

	this->iPrevCommand = 0;

	return;
}

bool player::tick()
	throw (std::ios::failure)
{
	if (this->data.eof()) return false;

	// Read in the number of ticks until the next event
	uint32_t iDelay = this->readMIDINumber();

	// Wait for the required delay
	//if (iDelay) this->pOPL->updateBlock((iDelay * AUD_FREQ) / this->cmfHeader.iTicksPerSecond);
	if (iDelay) this->cbDelay((iDelay * 1000) / this->cmfHeader.iTicksPerSecond);

	// Read in the next event
	uint8_t iCommand;
	this->data >> u8(iCommand);
	if (iCommand & 0x80) {
		this->iPrevCommand = iCommand;
	} else {
		// Running status, use previous command
		this->data.seekg(-1, std::ios::cur); // dodgy, fix this
		iCommand = this->iPrevCommand;
	}

		if (!(iCommand & 0x80)) {
			std::cout << "Corrupt CMF file or bug in MIDI parser - invalid MIDI event "
				<< (int)iCommand << " at offset 0x" << std::hex << this->data.tellg()
				<< std::endl;
			return false;
		}

		uint8_t iChannel = iCommand & 0x0F;
		switch (iCommand & 0xF0) {
			case 0x80: { // Note off (two data bytes)
				uint8_t iNote, iVelocity;
				this->data
					>> u8(iNote)
					>> u8(iVelocity)  // release velocity
				;
				this->cmfNoteOff(iChannel, iNote, iVelocity);
				break;
			}
			case 0x90: { // Note on (two data bytes)
				uint8_t iNote, iVelocity;
				this->data
					>> u8(iNote)
					>> u8(iVelocity)  // attack velocity
				;
				if (iVelocity) {
					this->cmfNoteOn(iChannel, iNote, iVelocity);
				} else {
					// This is a note-off instead (velocity == 0)
					this->cmfNoteOff(iChannel, iNote, iVelocity); // 64 is the MIDI default note-off velocity
					break;
				}
				break;
			}
			case 0xA0: { // Polyphonic key pressure (two data bytes)
				uint8_t iNote, iPressure;
				this->data
					>> u8(iNote)
					>> u8(iPressure)
				;
				std::cout << "Key pressure not yet implemented!" << std::endl;
				break;
			}
			case 0xB0: { // Controller (two data bytes)
				uint8_t iController, iValue;
				this->data
					>> u8(iController)
					>> u8(iValue)
				;
				this->MIDIcontroller(iChannel, iController, iValue);
				break;
			}
			case 0xC0: { // Instrument change (one data byte)
				uint8_t iNewInstrument;
				this->data
					>> u8(iNewInstrument)
				;
				this->chMIDI[iChannel].iPatch = iNewInstrument;
				std::cout << "Remembering MIDI channel " << (int)iChannel << " now uses patch " << (int)iNewInstrument << std::endl;
				//this->MIDIchangeInstrument(iChannel, iNewInstrument);
				break;
			}
			case 0xD0: { // Channel pressure (one data byte)
				uint8_t iPressure;
				this->data
					>> u8(iPressure)
				;
				std::cout << "Channel pressure not yet implemented!" << std::endl;
				break;
			}
			case 0xE0: { // Pitch bend (two data bytes)
				uint8_t iLSB, iMSB;
				this->data
					>> u8(iLSB)
					>> u8(iMSB)
				;
				// Only lower seven bits are used in each byte
				uint16_t iValue = ((iMSB & 0x7F) << 7) | (iLSB & 0x7F);
				// 8192 is middle, 0 is -2 semitones, 16384 is +2 semitones
				this->chMIDI[iChannel].iPitchbend = iValue;
				std::cout << "Channel " << (int)(iChannel + 1) << " pitchbent to " << iValue
					<< " (" << (float)(iValue - 8192) / 8192 << ")" << std::endl;
				break;
			}
			case 0xF0: // System message (arbitrary data bytes)
				switch (iCommand) {
					case 0xF0: { // Sysex
						uint8_t iNextByte;
						std::cout << "Sysex message: ";
						do {
							this->data >> u8(iNextByte);
							std::cout << std::hex << (int)iNextByte;
						} while ((iNextByte & 0x80) == 0);
						std::cout << std::endl;
						// This will have read in the terminating EOX (0xF7) message too
						break;
					}
					case 0xF1: // MIDI Time Code Quarter Frame
						this->data.seekg(1, std::ios::cur); // message data (ignored)
						break;
					case 0xF2: // Song position pointer
						this->data.seekg(2, std::ios::cur); // message data (ignored)
						break;
					case 0xF3: // Song select
						this->data.seekg(1, std::ios::cur); // message data (ignored)
						std::cout << "Warning: MIDI Song Select is not implemented." << std::endl;
						break;
					case 0xF6: // Tune request
						break;
					case 0xF7: // End of System Exclusive (EOX) - should never be read, should be absorbed by Sysex handling code
						break;

					// These messages are "real time", meaning they can be sent between the bytes of other messages - but we're
					// lazy and don't handle these here (hopefully they're not necessary in a MIDI file, and even less likely to
					// occur in a CMF.)
					case 0xF8: // Timing clock (sent 24 times per quarter note, only when playing)
					case 0xFA: // Start
					case 0xFB: // Continue
					case 0xFE: // Active sensing (sent every 300ms or MIDI connection assumed lost)
						break;
					case 0xFC: // Stop
						std::cout << "Received Real Time Stop message (0xFC)" << std::endl;
						return false;
					case 0xFF: { // System reset, used as meta-events in a MIDI file
						uint8_t iEvent;
						this->data >> u8(iEvent);
						switch (iEvent) {
							case 0x2F: // end of track
								std::cout << "Reached MIDI end-of-track" << std::endl;
								return false;
							default:
								std::cout << "Unknown MIDI meta-event 0xFF 0x" << std::hex << (int)iEvent << std::endl;
								break;
						}
						break;
					}
					default:
						std::cout << "Unknown MIDI system command 0x" << std::hex << (int)iCommand << std::endl;
						break;
				}
				break;
			default:
				std::cout << "Unknown MIDI command 0x" << std::hex << (int)iCommand << std::endl;
				break;
		}

	return true; // more data to play
}

// Read a variable-length integer from MIDI data
uint32_t player::readMIDINumber()
{
	uint32_t iValue = 0;
	for (int i = 0; i < 4; i++) {
		uint8_t iNext;
		this->data >> u8(iNext);
		iValue <<= 7;
		iValue |= (iNext & 0x7F); // ignore the MSB
		if ((iNext & 0x80) == 0) break; // last byte has the MSB unset
	}
	return iValue;
}

// iChannel: OPL channel (0-8)
// iOperator: 0 == Modulator, 1 == Carrier
//   Source - source operator to read from instrument definition
//   Dest - destination operator on OPL chip
// iInstrument: Index into this->pInstruments array of CMF instruments
void player::writeInstrumentSettings(uint8_t iChannel, uint8_t iOperatorSource, uint8_t iOperatorDest, uint8_t iInstrument)
{
	assert(iChannel <= 8);

	uint8_t iOPLOffset = OPLOFFSET(iChannel);
	if (iOperatorDest) iOPLOffset += 3; // Carrier if iOperator == 1 (else Modulator)

	this->setReg(BASE_CHAR_MULT + iOPLOffset, this->pInstruments[iInstrument].op[iOperatorSource].iCharMult);
	this->setReg(BASE_SCAL_LEVL + iOPLOffset, this->pInstruments[iInstrument].op[iOperatorSource].iScalingOutput);
	this->setReg(BASE_ATCK_DCAY + iOPLOffset, this->pInstruments[iInstrument].op[iOperatorSource].iAttackDecay);
	this->setReg(BASE_SUST_RLSE + iOPLOffset, this->pInstruments[iInstrument].op[iOperatorSource].iSustainRelease);
	this->setReg(BASE_WAVE      + iOPLOffset, this->pInstruments[iInstrument].op[iOperatorSource].iWaveSel);

	// TODO: Check to see whether we should only be loading this for one or both operators
	this->setReg(BASE_FEED_CONN + iChannel, this->pInstruments[iInstrument].iConnection);
	return;
}

// Write a byte to the OPL "chip" and update the current record of register states
void player::setReg(uint8_t iRegister, uint8_t iValue)
	throw ()
{
	this->cbSetRegister(iRegister, iValue);
	this->iCurrentRegs[iRegister] = iValue;
	return;
}

void player::cmfNoteOn(uint8_t iChannel, uint8_t iNote, uint8_t iVelocity)
{
	// Note 42 ==> FNum 485 blk 2 ==> 92.50640Hz
	// Get the OPL frequency of this MIDI note
	uint8_t iBlock = iNote / 12;
	if (iBlock > 1) iBlock--; // keep in the same range as the Creative player

	double d = pow(2, (
		(double)iNote + (
			(this->chMIDI[iChannel].iPitchbend - 8192) / 8192.0
		) + (
			this->iTranspose / 128
		) - 9) / 12.0 - (iBlock - 20))
		* 440.0 / 32.0 / 50000.0;
	uint16_t iOPLFNum = (uint16_t)(d+0.5);
	if (iOPLFNum > 1023) std::cout << "This song plays a note that is out of range! (send this song to malvineous@shikadi.net!)" << std::endl;

	// See if we're playing a rhythm mode percussive instrument
	if ((iChannel > 10) && (this->bPercussive)) {
		uint8_t iPercChannel = this->getPercChannel(iChannel);

		// Will have to set every time (easier) than figuring out whether the mod
		// or car needs to be changed.
		//if (this->chOPL[iPercChannel].iMIDIPatch != this->chMIDI[iChannel].iPatch) {
			this->MIDIchangeInstrument(iPercChannel, iChannel, this->chMIDI[iChannel].iPatch);
		//}

		/*  Velocity calculations - TODO: Work out the proper formula

		iVelocity -> iLevel  (values generated by Creative's player)
		7f -> 00
		7c -> 00

		7b -> 09
		73 -> 0a
		6b -> 0b
		63 -> 0c
		5b -> 0d
		53 -> 0e
		4b -> 0f
		43 -> 10
		3b -> 11
		33 -> 13
		2b -> 15
		23 -> 19
		1b -> 1b
		13 -> 1d
		0b -> 1f
		03 -> 21

		02 -> 21
		00 -> N/A (note off)
		*/
		// Approximate formula, need to figure out more accurate one (my maths isn't so good...)
		int iLevel = 0x25 - sqrt(iVelocity * 16/*6*/);//(127 - iVelocity) * 0x20 / 127;
		if (iVelocity > 0x7b) iLevel = 0; // full volume
		if (iLevel < 0) iLevel = 0;
		if (iLevel > 0x3F) iLevel = 0x3F;
		//if (iVelocity < 0x40) iLevel = 0x10;

		int iOPLOffset = BASE_SCAL_LEVL + OPLOFFSET(iPercChannel);
		//if ((iChannel == 11) || (iChannel == 12) || (iChannel == 14)) {
		if (iChannel == 11) iOPLOffset += 3; // only do bassdrum carrier for volume control
			//iOPLOffset += 3; // carrier
			this->setReg(iOPLOffset, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | iLevel);//(iVelocity * 0x3F / 127));
		//}
		// Bass drum (ch11) uses both operators
		//if (iChannel == 11) this->setReg(iOPLOffset + 3, (this->iCurrentRegs[iOPLOffset + 3] & ~0x3F) | iLevel);

		#ifdef USE_VELOCITY  // Official CMF player seems to ignore velocity levels
			uint16_t iLevel = 0x2F - (iVelocity * 0x2F / 127); // 0x2F should be 0x3F but it's too quiet then
			//printf("%02X + vel %d (lev %02X) == %02X\n", this->iCurrentRegs[iOPLOffset], iVelocity, iLevel, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | iLevel);
			//this->setReg(iOPLOffset, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | (0x3F - (iVelocity >> 1)));//(iVelocity * 0x3F / 127));
			this->setReg(iOPLOffset, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | iLevel);//(iVelocity * 0x3F / 127));
		#endif

		// Apparently you can't set the frequency for the cymbal or hihat?
		// Vinyl requires you don't set it, Kiloblaster requires you do!
		this->setReg(BASE_FNUM_L + iPercChannel, iOPLFNum & 0xFF);
		this->setReg(BASE_KEYON_FREQ + iPercChannel, (iBlock << 2) | ((iOPLFNum >> 8) & 0x03));

		uint8_t iBit = 1 << (15 - iChannel);

		// Turn the perc instrument off if it's already playing (OPL can't do
		// polyphonic notes w/ percussion)
		if (this->iCurrentRegs[BASE_RHYTHM] & iBit) this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~iBit);

		// I wonder whether we need to delay or anything here?

		// Turn the note on
		//if (iChannel == 15) {
		this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] | iBit);
		//logerror("CMF: Note %d on MIDI channel %d (mapped to OPL channel %d-1) - vel %02X, fnum %d/%d\n", iNote, iChannel, iPercChannel+1, iVelocity, iOPLFNum, iBlock);
		//}

		this->chOPL[iPercChannel].iNoteStart = ++this->iNoteCount;
		this->chOPL[iPercChannel].iMIDIChannel = iChannel;
		this->chOPL[iPercChannel].iMIDINote = iNote;

	} else { // Non rhythm-mode or a normal instrument channel

		// Figure out which OPL channel to play this note on
		int iOPLChannel = -1;
		int iNumChannels = this->bPercussive ? 6 : 9;
		for (int i = iNumChannels - 1; i >= 0; i--) {
			// If there's no note playing on this OPL channel, use that
			if (this->chOPL[i].iNoteStart == 0) {
				iOPLChannel = i;
				// See if this channel is already set to the instrument we want.
				if (this->chOPL[i].iMIDIPatch == this->chMIDI[iChannel].iPatch) {
					// It is, so stop searching
					break;
				} // else keep searching just in case there's a better match
			}
		}
		if (iOPLChannel == -1) {
			// All channels were in use, find the one with the longest note
			iOPLChannel = 0;
			int iEarliest = this->chOPL[0].iNoteStart;
			for (int i = 1; i < iNumChannels; i++) {
				if (this->chOPL[i].iNoteStart < iEarliest) {
					// Found a channel with a note being played for longer
					iOPLChannel = i;
					iEarliest = this->chOPL[i].iNoteStart;
				}
			}
			std::cout << "Warning: Too many polyphonic notes, cutting note on "
				"channel " << iOPLChannel << std::endl;
		}

		// Run through all the channels with negative notestart values - these
		// channels have had notes recently stop - and increment the counter
		// to slowly move the channel closer to being reused for a future note.
		//for (int i = 0; i < iNumChannels; i++) {
		//	if (this->chOPL[i].iNoteStart < 0) this->chOPL[i].iNoteStart++;
		//}

		// Now the new note should be played on iOPLChannel, but see if the instrument
		// is right first.
		if (this->chOPL[iOPLChannel].iMIDIPatch != this->chMIDI[iChannel].iPatch) {
			this->MIDIchangeInstrument(iOPLChannel, iChannel, this->chMIDI[iChannel].iPatch);
		}

		this->chOPL[iOPLChannel].iNoteStart = ++this->iNoteCount;
		this->chOPL[iOPLChannel].iMIDIChannel = iChannel;
		this->chOPL[iOPLChannel].iMIDINote = iNote;
/*					-- This seems quite normal, a lot of songs don't always use noteoffs between notes
          -- Actually, at least one song (xargon1\song_9.cmf) won't work unless noteoffs are sent before noteons,
             because that song goes "note1on, note2on, note1off, note2off" so you have to switch the notes off
             in order!
*/
//// if (this->iCurrentRegs[BASE_KEYON_FREQ + iChannel] & OPLBIT_KEYON) {
//							fprintf(stderr, "CMF: Note-on when note is already on!\n");
////				this->setReg(BASE_KEYON_FREQ + iOPLChannel, this->iCurrentRegs[BASE_KEYON_FREQ + iOPLChannel] & ~OPLBIT_KEYON);
			//}

			/*fprintf(stderr, "Chan %d freq %lf - %02X: %02X, %02X: %02X\n", iChannel, dbFreq,
				BASE_FNUM_L + iChannel, iOPLFNum & 0xFF,
				BASE_KEYON_FREQ + iChannel, OPLBIT_KEYON | (iBlock << 2) | ((iOPLFNum & 0x300) >> 8)
			);*/

			#ifdef USE_VELOCITY  // Official CMF player seems to ignore velocity levels
				// Adjust the channel volume to match the note velocity
				uint8_t iOPLOffset = BASE_SCAL_LEVL + OPLOFFSET(iChannel) + 3; // +3 == Carrier
				uint16_t iLevel = 0x00;//0x2F - (iVelocity * 0x2F / 127); // 0x2F should be 0x3F but it's too quiet then
				//if (iVelocity < 0x40) iLevel = 0x10;
				//printf("%02X + vel %d (lev %02X) == %02X\n", this->iCurrentRegs[iOPLOffset], iVelocity, iLevel, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | iLevel);
				//this->setReg(iOPLOffset, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | (0x3F - (iVelocity >> 1)));//(iVelocity * 0x3F / 127));
				this->setReg(iOPLOffset, (this->iCurrentRegs[iOPLOffset] & ~0x3F) | iLevel);//(iVelocity * 0x3F / 127));
			#endif

			// Set the frequency and play the note
			this->setReg(BASE_FNUM_L + iOPLChannel, iOPLFNum & 0xFF);
			//if (iChannel == 5)
				this->setReg(BASE_KEYON_FREQ + iOPLChannel, OPLBIT_KEYON | (iBlock << 2) | ((iOPLFNum & 0x300) >> 8));
			//	logerror("CMF: Note %d on MIDI channel %d (mapped to OPL channel %d)\n", iNote, iChannel, iOPLChannel);
			//else
			//	this->setReg(BASE_KEYON_FREQ + iOPLChannel, /* TEMP - no keyon */ (iBlock << 2) | ((iOPLFNum & 0x300) >> 8));
		//}
	}
	return;
}

void player::cmfNoteOff(uint8_t iChannel, uint8_t iNote, uint8_t iVelocity)
{
	if ((iChannel > 10) && (this->bPercussive)) {
		int iOPLChannel = this->getPercChannel(iChannel);
		if (this->chOPL[iOPLChannel].iMIDINote != iNote) return; // there's a different note playing now
		this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~(1 << (15 - iChannel)));
		/*switch (iChannel) {
			case 11: // Bass drum (operator 13+16 == channel 7 modulator+carrier)
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x10);
				break;
			case 12: // Snare drum (operator 17 == channel 8 carrier)
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x08);
				break;
			case 13: // Tom tom (operator 15 == channel 9 modulator)
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x04);
				break;
			case 14: // Top cymbal (operator 18 == channel 9 carrier)
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x02);
				break;
			case 15: // Hi-hat (operator 14 == channel 8 modulator)
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x01);
				break;
		}*/
		this->chOPL[iOPLChannel].iNoteStart = 0; // channel free
	} else { // Non rhythm-mode or a normal instrument channel
		int iOPLChannel = -1;
		int iNumChannels = this->bPercussive ? 6 : 9;
		for (int i = 0; i < iNumChannels; i++) {
			if (
				(this->chOPL[i].iMIDIChannel == iChannel) &&
				(this->chOPL[i].iMIDINote == iNote) &&
				(this->chOPL[i].iNoteStart != 0)
			) {
				// Found the note, switch it off
				//logerror("CMF: Noteoff on note %d, chan %d\n", iNote, iChannel);
				this->chOPL[i].iNoteStart = 0;
				iOPLChannel = i;
				break;
			}
		}

		if (iOPLChannel == -1) {
			//logerror("CMF: Tried to switch off note %d on chan %d but couldn't find it!\n", iNote, iChannel);
			/*for (int i = 0; i < iNumChannels; i++) {
				logerror("CMF: Notelist: OPLCH %d: Note %d, MIDICH %d\n", i, this->chOPL[i].iMIDINote, this->chOPL[i].iMIDIChannel);
			}*/
			return;
		}

		this->setReg(BASE_KEYON_FREQ + iOPLChannel, this->iCurrentRegs[BASE_KEYON_FREQ + iOPLChannel] & ~OPLBIT_KEYON);
	}
	return;
}

uint8_t player::getPercChannel(uint8_t iChannel)
{
	switch (iChannel) {
		case 11: return 7-1; // Bass drum
		case 12: return 8-1; // Snare drum
		case 13: return 9-1; // Tom tom
		case 14: return 9-1; // Top cymbal
		case 15: return 8-1; // Hihat
	}
	std::cerr << "ERROR: Tried to get the percussion channel from MIDI "
		"channel " << iChannel << " - this shouldn't happen!" << std::endl;
	return 0;
}

void player::MIDIchangeInstrument(uint8_t iOPLChannel, uint8_t iMIDIChannel, uint8_t iNewInstrument)
{
	std::cout << "OPL channel " << (int)(iOPLChannel + 1) << "-1 (MIDI channel "
		<< (int)iMIDIChannel << ") -> MIDI instrument " << (int)iNewInstrument
		<< std::endl;
	if ((iMIDIChannel > 10) && (this->bPercussive)) {
		switch (iMIDIChannel) {
			case 11: // Bass drum (operator 13+16 == channel 7 modulator+carrier)
				writeInstrumentSettings(7-1, 0, 0, iNewInstrument);
				writeInstrumentSettings(7-1, 1, 1, iNewInstrument);
				break;
			case 12: // Snare drum (operator 17 == channel 8 carrier)
			//case 15:
				writeInstrumentSettings(8-1, 0, 1, iNewInstrument);

				//
				//writeInstrumentSettings(8-1, 0, 0, iNewInstrument);
				break;
			case 13: // Tom tom (operator 15 == channel 9 modulator)
			//case 14:
				writeInstrumentSettings(9-1, 0, 0, iNewInstrument);

				//
				//writeInstrumentSettings(9-1, 0, 1, iNewInstrument);
				break;
			case 14: // Top cymbal (operator 18 == channel 9 carrier)
				writeInstrumentSettings(9-1, 0, 1, iNewInstrument);
				break;
			case 15: // Hi-hat (operator 14 == channel 8 modulator)
				writeInstrumentSettings(8-1, 0, 0, iNewInstrument);
				break;
			default:
				std::cout << "Invalid MIDI channel " << (int)(iMIDIChannel + 1) << " (not melodic and not percussive!)" << std::endl;
				break;
		}
		this->chOPL[iOPLChannel].iMIDIPatch = iNewInstrument;
	} else {
		// Standard nine OPL channels
		writeInstrumentSettings(iOPLChannel, 0, 0, iNewInstrument);
		writeInstrumentSettings(iOPLChannel, 1, 1, iNewInstrument);
		this->chOPL[iOPLChannel].iMIDIPatch = iNewInstrument;
	}
	return;
}

void player::MIDIcontroller(uint8_t iChannel, uint8_t iController, uint8_t iValue)
{
	switch (iController) {
		case 0x63:
			// Custom extension to allow CMF files to switch the AM+VIB depth on and
			// off (officially both are on, and there's no way to switch them off.)
			// Controller values:
			//   0 == AM+VIB off
			//   1 == VIB on
			//   2 == AM on
			//   3 == AM+VIB on
			if (iValue) {
				this->setReg(BASE_RHYTHM, (this->iCurrentRegs[BASE_RHYTHM] & ~0xC0) | (iValue << 6)); // switch AM+VIB extension on
			} else {
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0xC0); // switch AM+VIB extension off
			}
			std::cout << "CMF: AM+VIB depth change - AM "
				<< ((this->iCurrentRegs[BASE_RHYTHM] & 0x80) ? "on" : "off")
				<< ", VIB " << ((this->iCurrentRegs[BASE_RHYTHM] & 0x40) ? "on" : "off")
				<< std::endl;
			break;
		case 0x66:
			std::cout << "Song set marker to 0x" << std::hex << (int)iValue << std::endl;
			break;
		case 0x67:
			this->bPercussive = (iValue != 0);
			if (this->bPercussive) {
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] | 0x20); // switch rhythm-mode on
			} else {
				this->setReg(BASE_RHYTHM, this->iCurrentRegs[BASE_RHYTHM] & ~0x20); // switch rhythm-mode off
			}
			std::cout << "Percussive/rhythm mode " << (this->bPercussive ? "enabled" : "disabled") << std::endl;
			break;
		case 0x68:
			// TODO: Shouldn't this just affect the one channel, not the whole song?  -- have pitchbends for that
//						this->dbAFreq += pow(2, (iValue/128.0)/12.0);// * (double)iValue;// / 128;
			//this->dbAFreq = 440.0 + pow(2, 1/12.0) * (double)iValue / 128.0;
			this->iTranspose = iValue;
			std::cout << "Transposing all notes up by " << (int)iValue << " * 1/128ths of a semitone" << std::endl;
			break;
		case 0x69:
//						this->dbAFreq -= pow(2, (iValue/128.0)/12.0);// * (double)iValue;// / 128;
//						this->dbAFreq -= pow(2, 1/12.0) * (double)iValue;// / 128.0;
			//this->dbAFreq = 440.0 - pow(2, 1/12.0) * (double)iValue / 128.0;
			this->iTranspose = -iValue;
			std::cout << "Transposing all notes down by " << (int)iValue << " * 1/128ths of a semitone" << std::endl;
			break;
		default:
			std::cout << "Unsupported MIDI controller 0x" << std::hex << (int)iController << ", ignoring" << std::endl;;
			break;
	}
	return;
}

} // namespace cmf
