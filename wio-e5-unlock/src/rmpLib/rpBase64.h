//
// Created by daver on 3/20/2025.
//

#ifndef RPBASE64_H
#define RPBASE64_H

class rpBase64 {
public:

    static void Encode(const unsigned char *inSource, int inSrcLength,unsigned char *outDest, int &outDstLength);
    static void Decode(unsigned char *inSource, int inSrcLength, unsigned char *outDest, int &outDstLength);

private:

    static void EncodeBlock(const unsigned char in[3], unsigned char out[4], int len );
    static void DecodeBlock( unsigned char in[4], unsigned char out[3] );
};



#endif //RPBASE64_H
