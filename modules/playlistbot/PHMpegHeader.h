/*
 *  PHMpegHeader.h
 *  md5head
 *
 *  Created by Holger Bornträger on 11/08/07.
 *
 *  $Revision$
 */

#ifndef PHMPEGHEADER_H
#define PHMPEGHEADER_H
#include <iostream>
#include <string>

class PHMpegHeader {
	friend bool operator==(const PHMpegHeader &one,const PHMpegHeader &two);
	//friend inline bool operator!=(PHMpegHeader &one,PHMpegHeader &two);
	private:
		int mpegVersion;
		int layer;
		bool crc;
		unsigned int bitrate;
		unsigned int samplingFrequency;
		bool padded;
		bool privateBit;
		std::string channelMode;
		std::string modeExtension;
		bool copyright;
		bool original;
		std::string emphasis;
		unsigned char byteHeader[4];

		void SetMpegVersionFromHeader(unsigned char* header);
		void SetLayerFromHeader(unsigned char* header);
		void SetCRCFromHeader(unsigned char* header);
		void SetBitrateFromHeader(unsigned char* header);
		void SetSamplingFrequencyFromHeader(unsigned char* header);
		void SetPaddedFromHeader(unsigned char* header);
		void SetPrivateBitFromHeader(unsigned char* header);
		void SetChannelModeFromHeader(unsigned char* header);
		void SetModeExtensionFromHeader(unsigned char* header);
		void SetCopyrightFromHeader(unsigned char* header);
		void SetOriginalFromHeader(unsigned char* header);
		void SetEmphasisFromHeader(unsigned char* header);

	public:
		PHMpegHeader();
		PHMpegHeader(unsigned char* header);
		PHMpegHeader(const PHMpegHeader &two);
		~PHMpegHeader();
		void Print(std::ostream &o);
		void InterpretHeader(unsigned char* header);
		unsigned char* GetByteHeader() const;
		bool IsValidHeader();
		unsigned int GetPacketSize();
		double GetTime();
};

inline bool operator==(const PHMpegHeader &one,const PHMpegHeader &two){

	return	(one.mpegVersion == two.mpegVersion &&
			one.layer == two.layer &&
			one.crc == two.crc &&
			one.bitrate == two.bitrate &&
			one.samplingFrequency == two.samplingFrequency &&
			one.padded == two.padded &&
			//one.privateBit == two.privateBit && // doesnt matter for us
			one.channelMode == two.channelMode &&
			one.modeExtension == two.modeExtension &&
			one.copyright == two.copyright &&
			one.original == two.original &&
			one.emphasis == two.emphasis);
}

inline bool operator!=(const PHMpegHeader &one,const PHMpegHeader &two){
	return !(one == two);
}



#endif
