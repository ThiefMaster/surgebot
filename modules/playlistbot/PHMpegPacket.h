/*
 *  PHMpegPacket.h
 *  mpegInterleave
 *
 *  Created by Holger Bornträger on 11/25/07.
 *  Copyright 2007 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef PHMPEGPACKET_H
#define PHMPEGPACKET_H


#include "PHMpegHeader.h"
#include <string.h>

class PHMpegPacket{
	protected:
		PHMpegHeader header;
		unsigned char* body;
		unsigned int size;
	public:
		PHMpegPacket(){};
		PHMpegPacket(PHMpegHeader _header,unsigned char* _body,unsigned int _size){
			header = _header;
			size = _size;
			body = new unsigned char[size];
			memcpy(body,_body,size);
		}

		//CopyConstructor

		PHMpegPacket(const PHMpegPacket &packet){
			header = packet.header;
			size = packet.size;
			body = new unsigned char[packet.size];
			memcpy(body,packet.body,size);
		}

		~PHMpegPacket(){
			delete[] body;
		}

		double GetTime() {
			return header.GetTime();
		}

		unsigned char* GetBytePacket(){
			unsigned char* buffer = new unsigned char[size];
			memcpy(buffer,body,size);
			return buffer;
		}

		unsigned int GetSize(){
			return size;
		}

		PHMpegHeader GetHeader(){
			return header;
		}

		void Print(std::ostream &o){
			o.write((char*)(body),size);
		}

		//memleak but easy  dropping of the last byte(s)
		void SetSize(unsigned int newsize){
			size = newsize;
		}

};


inline std::ostream &operator<<(std::ostream &o,PHMpegPacket &packet){
	packet.Print(o);
	return o;
}



#endif
