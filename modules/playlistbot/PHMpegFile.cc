/*
 *  PHMpegFile.cpp
 *  mpegInterleave
 *
 *  Created by Holger Bornträger on 11/18/07.
 *  Copyright 2007 __MyCompanyName__. All rights reserved.
 *
 */

#include <string.h>
#include "PHMpegFile.h"
#include <fstream>
#include "PHMpegPacket.h"

PHMpegFile::~PHMpegFile(){
	current = packets.begin();
	while (current != packets.end()) {
		delete *current;
		++current;
	}
}

PHMpegFile::PHMpegFile(std::string fileName){
	time = 0;
	unsigned char buffer[4] = {0x00,0x00,0x00,0x00};
	std::ifstream mpegFile(fileName.c_str());
	mpegFile.read((char*)buffer,4);
	unsigned char *buffer2;
	unsigned char *copybuffer;
	unsigned int pos = 0;
	PHMpegHeader header(buffer);
	while (!header.IsValidHeader()){
		buffer[0]=buffer[1];
		buffer[1]=buffer[2];
		buffer[2]=buffer[3];
		mpegFile.read((char*)buffer+3,1);
		header.InterpretHeader(buffer);
		pos++;
	}
	while (!mpegFile.eof()){
		while ((!header.IsValidHeader()) && !mpegFile.eof()){
			buffer[0]=buffer[1];
			buffer[1]=buffer[2];
			buffer[2]=buffer[3];
			mpegFile.read((char*)buffer+3,1);
			header.InterpretHeader(buffer);
			pos ++ ;
		}
		if (!mpegFile.eof()) {
			unsigned int size = header.GetPacketSize();
			buffer2 = new unsigned char[size];
			copybuffer = header.GetByteHeader();
			memcpy(buffer2,copybuffer,4);
			delete[] copybuffer;
			mpegFile.read((char*)buffer2+4,header.GetPacketSize()-4);
			if (mpegFile.eof()) break;
			pos += size;
			packets.push_back(new PHMpegPacket(header,buffer2,size)); // one step further
			time += header.GetTime();
			mpegFile.read((char*)buffer,4);
			header.InterpretHeader(buffer);
			delete[] buffer2;
		}
	}
	current = packets.begin();
}



void PHMpegFile::ResetPosition(){
	current = packets.begin();
}

PHMpegPacket PHMpegFile::GetNextPacket(){

	return *(*(current++));
}

void PHMpegFile::WriteToFile(std::string fileName){
	std::ofstream mpegFile(fileName.c_str());

	for ( std::vector<PHMpegPacket*>::iterator runner = packets.begin();runner < packets.end(); ++runner){
			mpegFile << *(*runner);
	}
}

PHMpegHeader PHMpegFile::GetHeader(){
	return ((*(packets.begin()))->GetHeader());
}

bool PHMpegFile::Eof(){
	return (current == packets.end());
}

void PHMpegFile::AddPacket(PHMpegPacket packet){
	PHMpegPacket* buffer = new PHMpegPacket(packet);
	packets.push_back(buffer);
}

double PHMpegFile::GetTime(){
	return time;
}
