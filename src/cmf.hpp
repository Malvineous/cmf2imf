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
 */

#ifndef CMF_HPP_
#define CMF_HPP_

#include <boost/bind.hpp>
#include <iostream>
#include <stdint.h>

namespace cmf {

/// Set an OPL register to a given value
typedef boost::function<void(uint8_t, uint8_t)> FN_SETREGISTER;
//typedef void (*FN_SETREGISTER)(uint8_t reg, uint8_t val);

/// Wait for the given number of ticks
//typedef void (*FN_DELAY)(uint16_t ticks);
typedef boost::function<void(uint16_t)> FN_DELAY;

typedef struct {
	uint16_t iInstrumentBlockOffset;
	uint16_t iMusicOffset;
	uint16_t iTicksPerQuarterNote;
	uint16_t iTicksPerSecond;
	uint16_t iTagOffsetTitle;
	uint16_t iTagOffsetComposer;
	uint16_t iTagOffsetRemarks;
	uint8_t iChannelsInUse[16];
	uint16_t iNumInstruments;
	uint16_t iTempo;
} CMFHEADER;

typedef struct {
	uint8_t iCharMult;
	uint8_t iScalingOutput;
	uint8_t iAttackDecay;
	uint8_t iSustainRelease;
	uint8_t iWaveSel;
} OPERATOR;

typedef struct {
	OPERATOR op[2]; // 0 == modulator, 1 == carrier
	uint8_t iConnection;
} SBI;

typedef struct {
	int iPatch; // MIDI patch for this channel
	int iPitchbend; // Current pitchbend amount for this channel
} MIDICHANNEL;

typedef struct {
	int iNoteStart;   // When the note started playing (longest notes get cut first, 0 == channel free)
	int iMIDINote;    // MIDI note number currently being played on this OPL channel
	int iMIDIChannel; // Source MIDI channel where this note came from
	int iMIDIPatch;   // Current MIDI patch set on this OPL channel
} OPLCHANNEL;

class player {
	private:
		std::istream &data;
		FN_SETREGISTER cbSetRegister;
		FN_DELAY cbDelay;
		uint32_t iPlayPointer;		// Current location of playback pointer
		CMFHEADER cmfHeader;
		SBI *pInstruments;
		bool bPercussive; // are rhythm-mode instruments enabled?
		uint8_t iCurrentRegs[256]; // Current values in the OPL chip
		int iTranspose;  // Transpose amount for entire song (between -128 and +128)
		uint8_t iPrevCommand; // Previous command (used for repeated MIDI commands, as the seek and playback code need to share this)

		int iNoteCount;  // Used to count how long notes have been playing for
		MIDICHANNEL chMIDI[16];
		OPLCHANNEL chOPL[9];

	public:
		player(std::istream& data, FN_SETREGISTER cbSetRegister, FN_DELAY cbDelay)
			throw (std::ios::failure);
		virtual ~player()
			throw ();

		/// Preload instruments and seek to start of song.
		void init()
			throw (std::ios::failure);

		/// Send the next lot of data.
		/**
		 * @return true if more data to play, false if end of file/song reached.
		 */
		bool tick()
			throw (std::ios::failure);

	protected:
		uint32_t readMIDINumber();
		void writeInstrumentSettings(uint8_t iChannel, uint8_t iOperatorSource, uint8_t iOperatorDest, uint8_t iInstrument);

		/// Write a byte to the OPL "chip" and update the current record of register states
		void setReg(uint8_t iRegister, uint8_t iValue)
			throw ();

		void cmfNoteOn(uint8_t iChannel, uint8_t iNote, uint8_t iVelocity);
		void cmfNoteOff(uint8_t iChannel, uint8_t iNote, uint8_t iVelocity);

		/// When a MIDI instrument is played on a percussive channel (e.g. 11), figure
		/// out which OPL rhythm-mode channel it must be played on (e.g. 7)
		uint8_t getPercChannel(uint8_t iChannel);

		/// Change instrument
		void MIDIchangeInstrument(uint8_t iOPLChannel, uint8_t iMIDIChannel, uint8_t iNewInstrument);

		/// Controller (enable rhythm mode, etc.)
		void MIDIcontroller(uint8_t iChannel, uint8_t iController, uint8_t iValue);

};

} // namespace cmf

#endif // CMF_HPP_
