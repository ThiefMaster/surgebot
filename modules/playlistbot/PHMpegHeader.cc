/*
 *  PHMpegHeader.cpp
 *  md5head
 *
 *  Created by Holger Bornträger on 11/07/07.
 *
 *  $Revision$
 */

#include <string.h>
#include "PHMpegHeader.h"

PHMpegHeader::PHMpegHeader(){
	mpegVersion = 0;
	layer = 0;
	crc = false;
	bitrate = 0;
	samplingFrequency = 0;
	padded = false;
	privateBit = false;
	privateBit = false;
	channelMode = "";
	modeExtension = "";
	copyright = false;
	original = false;
}

PHMpegHeader::PHMpegHeader(unsigned char* header){
	InterpretHeader(header);
}

PHMpegHeader::PHMpegHeader(const PHMpegHeader &two){
			this->mpegVersion = two.mpegVersion;
			this->layer = two.layer;
			this->crc = two.crc;
			this->bitrate = two.bitrate;
			this->samplingFrequency = two.samplingFrequency;
			this->padded = two.padded;
			this->privateBit = two.privateBit;
			this->channelMode = two.channelMode;
			this->modeExtension = two.modeExtension;
			this->copyright = two.copyright;
			this->original = two.original;
			this->emphasis = two.emphasis;
			memcpy(this->byteHeader,two.byteHeader,4);
}

PHMpegHeader::~PHMpegHeader(){
}

void PHMpegHeader::SetMpegVersionFromHeader(unsigned char* header){
	unsigned char byte2 = header[1];
	byte2 = byte2 & 0x08;
	if (byte2 == 0x00) {
		mpegVersion = 2;
	} else {
		mpegVersion = 1;
	}

}

void PHMpegHeader::SetLayerFromHeader(unsigned char* header){
	unsigned char byte2 = header[1];
	byte2 = byte2 & 0x06;
	switch (byte2){
		case 0x06: layer = 1; break;
		case 0x04: layer = 2; break;
		case 0x02: layer = 3; break;
		default: layer = 0;
	}
}

void PHMpegHeader::SetCRCFromHeader(unsigned char* header){
	unsigned char byte2 = header[1];
	byte2 = byte2 & 0x01;
	crc = byte2 != 0x01;
}

void PHMpegHeader::SetBitrateFromHeader(unsigned char* header){
	unsigned int V1L1[]={  0, 32, 64, 96,128,160,192,224,256,288,320,352,384,416,448,  0};
	unsigned int V1L2[]={  0, 32, 48, 56, 64, 80, 96,112,128,160,192,224,256,320,384,  0};
	unsigned int V1L3[]={  0, 32, 40, 48, 56, 64, 80, 96,112,128,160,192,224,256,320,  0};
	unsigned int V2L1[]={  0, 32, 48, 56, 64, 80, 96,112,128,144,160,176,192,224,256,  0};
	unsigned int V2L2[]={  0,  8, 16, 24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,  0};
	unsigned char byte3 = header[2];
	byte3 = byte3 & 0xF0;
	byte3 >>= 4;
	if (mpegVersion==1 && layer==1) bitrate = V1L1[byte3];
	if (mpegVersion==1 && layer==2) bitrate = V1L2[byte3];
	if (mpegVersion==1 && layer==3) bitrate = V1L3[byte3];
	if (mpegVersion==2 && layer==1) bitrate = V2L1[byte3];
	if (mpegVersion==2 && (layer==2 || layer==3) ) bitrate = V2L2[byte3];
}

void PHMpegHeader::SetSamplingFrequencyFromHeader(unsigned char* header){
	unsigned int V1[]={44100,18000,32000,00000};
	unsigned int V2[]={22050,24000,16000,00000};
	unsigned char byte3 = header[2];
	byte3 = byte3 & 0x0C;
	byte3 >>= 2;
	if (mpegVersion == 1) samplingFrequency = V1[byte3];
	if (mpegVersion == 2) samplingFrequency = V2[byte3];
}

void PHMpegHeader::SetPaddedFromHeader(unsigned char* header){
	padded = (header[2]&0x02)==0x02;
}

void PHMpegHeader::SetPrivateBitFromHeader(unsigned char* header){
	privateBit = (header[2]&0x01)==0x01;
}

void PHMpegHeader::SetChannelModeFromHeader(unsigned char* header){
	unsigned char byte4 = header[3]&0xC0;
	if (byte4 == 0x00) channelMode = "Stereo";
	if (byte4 == 0x40) channelMode = "Joint stereo";
	if (byte4 == 0x80) channelMode = "Dual channel";
	if (byte4 == 0xC0) channelMode = "Single channel(Mono)";
}

void PHMpegHeader::SetModeExtensionFromHeader(unsigned char* header){
	unsigned char byte4 = header[3]&0x30;
	if (layer ==1 || layer ==2){
		if (byte4 == 0x00) modeExtension = "bands  4 to 31";
		if (byte4 == 0x10) modeExtension = "bands  8 to 31";
		if (byte4 == 0x20) modeExtension = "bands 12 to 31";
		if (byte4 == 0x30) modeExtension = "bands 16 to 31";
	}
	if (layer == 3){
		if (byte4 == 0x00) modeExtension = "";
		if (byte4 == 0x10) modeExtension = "Intensity stereo";
		if (byte4 == 0x20) modeExtension = "MS stereo";
		if (byte4 == 0x30) modeExtension = "Intensity stereo MS stereo";
	}
}

void PHMpegHeader::SetCopyrightFromHeader(unsigned char* header){
	copyright = (header[3]&0x08)==0x08;
}

void PHMpegHeader::SetOriginalFromHeader(unsigned char* header){
	original = (header[3]&0x04)==0x04;
}

void PHMpegHeader::SetEmphasisFromHeader(unsigned char* header){
	unsigned char byte4 = header[3]&0x03;
	if (byte4 == 0x00) emphasis = "none";
	if (byte4 == 0x01) emphasis = "50/15 ms";
	if (byte4 == 0x02) emphasis = "reserved emphasis (error)";
	if (byte4 == 0x03) emphasis = "CCIT J.17";
}

void PHMpegHeader::InterpretHeader(unsigned char* header){
	SetMpegVersionFromHeader(header);
	SetLayerFromHeader(header);
	SetCRCFromHeader(header);
	SetBitrateFromHeader(header);
	SetSamplingFrequencyFromHeader(header);
	SetPaddedFromHeader(header);
	SetPrivateBitFromHeader(header);
	SetChannelModeFromHeader(header);
	SetModeExtensionFromHeader(header);
	SetCopyrightFromHeader(header);
	SetOriginalFromHeader(header);
	SetEmphasisFromHeader(header);
	memcpy(byteHeader,header,4);
}

void PHMpegHeader::Print(std::ostream &o){
	o  <<	"  MPEG "			<<	mpegVersion				<<				'\n'	<<
			"  Layer "			<<	layer					<<				'\n'	<<
			"  Bitrate "		<<	bitrate					<<	"kbps"	<<	'\n'	<<
			"  Frequency "		<<	samplingFrequency		<<	"Hz"	<<	'\n'	<<
			"  Channel mode "	<<	channelMode				<<				'\n'	<<
			"  Extension "		<<	modeExtension			<<				'\n'	<<
			"  Emphasis "		<<	emphasis				<<				'\n'	<<
			"  CRC "			<<	(crc?"yes":"no")		<<				'\n'	<<
			"  padded "			<<	(padded?"yes":"no")		<<				'\n'	<<
			"  Copyrighted "	<<	(copyright?"yes":"no")	<<				'\n'	<<
			"  Original "		<<	(original?"yes":"no")	<<				'\n'	<<
			"  Private bit "	<<	(privateBit?"yes":"no")	<<				'\n';

}

bool PHMpegHeader::IsValidHeader(){
	return	(mpegVersion == 1 || mpegVersion ==2) &&
			(layer == 1 || layer == 2 || layer == 3) &&
			(bitrate != 0 && bitrate <=  448) &&
			byteHeader[0] == 0xFF &&
			(byteHeader[1] & 0xF0) == 0xF0 &&
			samplingFrequency != 0;
}

unsigned char* PHMpegHeader::GetByteHeader() const{
	unsigned char* returnBuffer = new unsigned char[4];
	memcpy(returnBuffer,byteHeader,4);
	return returnBuffer;
}

unsigned int  PHMpegHeader::GetPacketSize(){
	if (layer == 1){
		return (12 * bitrate*1000 / samplingFrequency + padded?1:0) * 4;
	}
	if ( layer ==2 || layer == 3){
		unsigned int padbuf = padded?1:0;
		unsigned int buffer = (144 * bitrate*1000) /samplingFrequency+padbuf;
		//printf (" testvalue: %i\n",buffer);
		return buffer;
	}
	return 0;
}

double PHMpegHeader::GetTime(){
	if (GetPacketSize() > 0) {
		return double((GetPacketSize())*8)/double(bitrate*1000);
	} else {
		return 0;
	}
}
