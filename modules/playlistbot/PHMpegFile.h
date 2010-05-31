/*
 *  PHMpegFile.h
 *  mpegInterleave
 *
 *  Created by Holger Bornträger on 11/18/07.
 *
 *  $Revision$
 */


#ifndef PHMPEGFILE_H
#define PHMPEGFILE_H

#include <vector>
#include <string>
#include "PHMpegPacket.h"

class PHMpegFile{
	private:
		std::vector<PHMpegPacket*> packets;
		std::vector<PHMpegPacket*>::iterator current;
		double time;
	public:
		PHMpegFile(){};
		PHMpegFile(const PHMpegFile &one){
			packets = one.packets;
			current = one.current;
		}
		PHMpegFile(std::string fileName);
		~PHMpegFile();
		void ResetPosition();
		PHMpegPacket GetNextPacket();
		bool Eof();
		void AddPacket(PHMpegPacket packet);
		void WriteToFile(std::string fileName);
		PHMpegHeader GetHeader();
		double GetTime();

};

#endif
