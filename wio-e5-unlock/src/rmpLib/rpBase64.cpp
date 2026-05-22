//
// Created by daver on 3/20/2025.
//

#include "rpBase64.h"

// Translation Table as described in RFC1113
static const unsigned char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Translation Table to decode
static const unsigned char cd64[]="|$$$}rstuvwxyz{$$$$$$$>?@ABCDEFGHIJKLMNOPQRSTUVW$$$$$$XYZ[\\]^_`abcdefghijklmnopq";


// encode 3 8-bit binary bytes as 4 '6-bit' characters
void rpBase64::EncodeBlock(const unsigned char in[3], unsigned char out[4], int len )
{
    out[0] = cb64[ in[0] >> 2 ];
    out[1] = cb64[ ((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4) ];
    out[2] = (unsigned char) (len > 1 ? cb64[ ((in[1] & 0x0f) << 2) | ((in[2] & 0xc0) >> 6) ] : '=');
    out[3] = (unsigned char) (len > 2 ? cb64[ in[2] & 0x3f ] : '=');
}


// decode 4 '6-bit' characters into 3 8-bit binary bytes
void rpBase64::DecodeBlock( unsigned char in[4], unsigned char out[3] )
{
    out[ 0 ] = (unsigned char ) (in[0] << 2 | in[1] >> 4);
    out[ 1 ] = (unsigned char ) (in[1] << 4 | in[2] >> 2);
    out[ 2 ] = (unsigned char ) (((in[2] << 6) & 0xc0) | in[3]);
}

// base64 encode adding padding and line breaks as per spec.
void rpBase64::Encode(const unsigned char *source, int inLength, unsigned char *dest, int &outlength )
{
    unsigned char in[3], out[4];
    int i, len, blocksout = 0;
    int inBytePos = 0;
    int outBytePos = 0;

    while( inBytePos < inLength )
    {
        len = 0;
        for( i = 0; i < 3; i++ ) {
            if( inBytePos < inLength ) {
                in[i] = source[inBytePos];
                inBytePos++;
                len++;
            }
             else {
                in[i] = 0;
            }
        }

        if( len ) {
            EncodeBlock( in, out, len );
            for( i = 0; i < 4; i++ ) {
                //putc( out[i], outfile );
                dest[outBytePos] = out[i];
                outBytePos++;
            }
            blocksout++;
        }

        /*if( blocksout >= (linesize/4) || feof( infile ) ) {
            if( blocksout ) {
                fprintf( outfile, "\r\n" );
            }
            blocksout = 0;
        } */
    }

    outlength = outBytePos;
}



// decode a base64 encoded stream discarding padding, line breaks and noise

void rpBase64::Decode( unsigned char *source, int src_length, unsigned char *dest, int &dst_length )
{
    unsigned char in[4], out[3], v;
    int i, len;
    int inBytePos = 0;
    int outBytePos = 0;
	bool bByteValid;

    while( inBytePos < src_length  )
    {
        for( len = 0, i = 0; i < 4 && inBytePos < src_length; i++ ) {
            v = 0;
            while( inBytePos < src_length && v == 0 ) {
				bByteValid = false;
                v = (unsigned char) source[inBytePos++];
				if( (v < 43 || v > 122) )
				{
					v = 0;
				}
				else
				{
					v = (unsigned char) cd64[ v - 43 ];
					bByteValid = true;
				}
                if( v )
				{
					if( v == '$' )
					{
						v = 0;
						bByteValid = false;
					}
					else
					{
						v = (unsigned char) v - 61;
						bByteValid = true;
					}
                }
            }
            if( bByteValid ) {
                len++;
                if( v ) {
                    in[ i ] = (unsigned char) (v - 1);
                }
            }
            else {
                in[i] = 0;
            }
        }

        if( len ) {
            DecodeBlock( in, out );
            for( i = 0; i < len - 1; i++ ) {
                 dest[outBytePos] = out[i];
                outBytePos++;
            }
        }

        dst_length = outBytePos;
    }
}


//-------