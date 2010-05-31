#include <string>
#include "PHMpegHeader.h"
#include "PHMpegFile.h"

extern "C" {
#include "mp3c.h"
}


extern "C" unsigned long mp3_playtime(const char *filename)
{
	std::string name(filename);
	PHMpegFile mpegFile(name);
	return (unsigned long) mpegFile.GetTime();
}

