/*
 * CMF2IMF - convert CMF files into id Software IMF files
 * Copyright (C) 2010 Adam Nielsen <malvineous@shikadi.net>
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
 */

#include <boost/program_options.hpp>
#include <boost/bind.hpp>
#include <iostream>
#include <fstream>
#include <camoto/iostream_helpers.hpp>

#include "cmf.hpp"

namespace po = boost::program_options;
using namespace camoto;

void setDelay(uint16_t delay, uint16_t *keep)
{
	*keep = delay;
	return;
}

void setRegister(uint8_t reg, uint8_t val, uint16_t *preDelay, std::ostream& out, int speed)
{
	// delay == milliseconds, 1000 == one second
	// if speed == 560, then 560 == one second
	// Convert delay ticks -> speed ticks
	unsigned long delay = (unsigned long)*preDelay * speed / 1000;

	out
		<< u16le(delay)
		<< u8(reg)
		<< u8(val)
	;
	*preDelay = 0;
	return;
}

int main(int argc, char *argv[])
{
	// Set a better exception handler
	std::set_terminate( __gnu_cxx::__verbose_terminate_handler );

	// Disable stdin/printf/etc. sync for a speed boost
	std::ios_base::sync_with_stdio(false);

	// Declare the supported options.
	po::options_description poOptions("Options");
	poOptions.add_options()
		("speed,s", po::value<int>(), "speed in Hertz (280, 560, 700)")
		("type,t",  po::value<int>(), "0 or 1 to create type-0 or type-1 IMF")
	;

	po::options_description poHidden("Hidden options");
	poHidden.add_options()
		("files", po::value< std::vector < std::string > >(), "input/output filenames")
		("help,h", "produce help message")
	;

	po::positional_options_description poPositional;
	poPositional.add("files", -1);

	po::options_description poComplete("Parameters");
	poComplete.add(poOptions).add(poHidden);

	po::variables_map vm;
	try {
		po::store(po::command_line_parser(argc, argv).
		options(poComplete).positional(poPositional).run(), vm);
		po::notify(vm);
	} catch (po::unknown_option& e) {
		std::cerr << "Command line error: " << e.what() << std::endl;
		return 1;
	} catch (std::exception& e) {
		std::cerr << "ERROR: " << e.what() << std::endl;
		return 1;
	}

	if (vm.count("help")) {
		std::cout <<
			"CMF2IMF - version 1.0, build date " __DATE__ " " __TIME__ << "\n"
			"Copyright (C) 2010 Adam Nielsen <malvineous@shikadi.net>\n"
			"This program comes with ABSOLUTELY NO WARRANTY.  This is free software,\n"
			"and you are welcome to change and redistribute it under certain conditions;\n"
			"see <http://www.gnu.org/licenses/> for details.\n"
			"\n"
			"Utility to convert Creative Labs' CMF files into id Software's IMF format.\n"
			"\n"
			"Usage: cmf2imf -s <speed> -t <imftype> cmffile imffile\n\n" << poOptions
			<< std::endl;
		return 0;
	}

	if (vm.count("speed") == 0) { std::cerr << "ERROR: No --speed option given, use --help for usage info." << std::endl; return 1; }
	if (vm.count("type")  == 0) { std::cerr << "ERROR: No --type option given, use --help for usage info."  << std::endl; return 1; }

	if (!vm.count("files")) {
		std::cerr << "ERROR: No filenames given, use --help for usage info." << std::endl;
		return 1;
	}

	const std::vector<std::string>& files = vm["files"].as< std::vector<std::string> >();
	if (files.size() == 1) {
		std::cerr << "ERROR: No output IMF filename given, use --help for usage info." << std::endl;
		return 1;
	} else if (files.size() != 2) {
		std::cerr << "ERROR: Too many filenames given, use --help for usage info." << std::endl;
		return 1;
	}

	int type = vm["type"].as<int>();

	std::cout << "Opening " << files[0] << std::endl;

	std::fstream infile(files[0].c_str(), std::ios::in | std::ios::binary);
	std::fstream outfile(files[1].c_str(), std::ios::out | std::ios::trunc | std::ios::binary);

	uint16_t delay = 0;
	cmf::FN_SETREGISTER fnSetReg = boost::bind<void>(setRegister,
		_1,
		_2,
		&delay,
		boost::ref(outfile),
		vm["speed"].as<int>()
	);
	cmf::FN_DELAY fnDelay = boost::bind<void>(setDelay,
		_1,
		&delay
	);

	// Insert some bytes to update later with the file length
	if (type == 1) outfile << u16le(0);

	// Initial bytes
	outfile << u16le(0);

	try {
		cmf::player p(infile, fnSetReg, fnDelay);
		p.init();
		while (p.tick()) { } ;
	} catch (std::ios::failure& e) {
		std::cerr << "ERROR: " << e.what() << std::endl;
		return 2;
	}

	if (type == 1) {
		// Update the file length at the start
		uint16_t size = outfile.tellp();
		size -= 2; // don't count field itself
		std::cout << "Updating type-1 header to file size " << size << std::endl;
		outfile.seekp(0, std::ios::beg);
		outfile << u16le(size);
	}

	std::cout << "Wrote " << files[1] << std::endl;
	return 0;
}
