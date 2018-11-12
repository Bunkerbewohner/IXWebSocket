/*
 * Copyright (c) 2015, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone, Inc. All rights reserved.
 *
 *  Adapted from websocketpp/extensions/permessage_deflate/enabled.hpp
 *  (same license as MZ: https://opensource.org/licenses/BSD-3-Clause)
 *
 *  - Reused zlib compression + decompression bits.
 *  - Refactored to have 2 class for compression and decompression, to allow multi-threading
 *    and make sure that _compressBuffer is not shared between threads.
 *  - Original code wasn't working for some reason, I had to add checks 
 *    for the presence of the kEmptyUncompressedBlock at the end of buffer so that servers
 *    would start accepting receiving/decoding compressed messages. Original code was probably
 *    modifying the passed in buffers before processing in enabled.hpp ?
 *  - Added more documentation.
 *
 *  Per message Deflate RFC: https://tools.ietf.org/html/rfc7692
 *  Chrome websocket -> https://github.com/chromium/chromium/tree/2ca8c5037021c9d2ecc00b787d58a31ed8fc8bcb/net/websockets
 *
 */

#include "IXWebSocketPerMessageDeflate.h"
#include "IXWebSocketPerMessageDeflateOptions.h"

#include <iostream>
#include <cassert>

namespace
{
    // The passed in size (4) is important, without it the string litteral
    // is treated as a char* and the null termination (\x00) makes it 
    // look like an empty string.
    const std::string kEmptyUncompressedBlock = std::string("\x00\x00\xff\xff", 4);

    const int kBufferSize = 1 << 14;
}

namespace ix
{
    //
    // Compressor
    //
    WebSocketPerMessageDeflateCompressor::WebSocketPerMessageDeflateCompressor()
      : _compressBufferSize(kBufferSize)
    {
        _deflateState.zalloc = Z_NULL;
        _deflateState.zfree = Z_NULL;
        _deflateState.opaque = Z_NULL;
    }

    WebSocketPerMessageDeflateCompressor::~WebSocketPerMessageDeflateCompressor()
    {
        deflateEnd(&_deflateState);
    }

    bool WebSocketPerMessageDeflateCompressor::init(uint8_t deflateBits,
                                                    bool clientNoContextTakeOver)
    {
        int ret = deflateInit2(
            &_deflateState,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            -1*deflateBits,
            4, // memory level 1-9
            Z_DEFAULT_STRATEGY
        );

        if (ret != Z_OK) return false;

        _compressBuffer.reset(new unsigned char[_compressBufferSize]);
        _flush = (clientNoContextTakeOver)
                 ? Z_FULL_FLUSH
                 : Z_SYNC_FLUSH;

        return true;
    }

    bool WebSocketPerMessageDeflateCompressor::endsWith(const std::string& value,
                                                        const std::string& ending)
    {
        if (ending.size() > value.size()) return false;
        return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
    }

    bool WebSocketPerMessageDeflateCompressor::compress(const std::string& in,
                                                        std::string& out)
    {
        //
        // 7.2.1.  Compression
        // 
        //    An endpoint uses the following algorithm to compress a message.
        // 
        //    1.  Compress all the octets of the payload of the message using
        //        DEFLATE.
        // 
        //    2.  If the resulting data does not end with an empty DEFLATE block
        //        with no compression (the "BTYPE" bits are set to 00), append an
        //        empty DEFLATE block with no compression to the tail end.
        // 
        //    3.  Remove 4 octets (that are 0x00 0x00 0xff 0xff) from the tail end.
        //        After this step, the last octet of the compressed data contains
        //        (possibly part of) the DEFLATE header bits with the "BTYPE" bits
        //        set to 00.
        //
        size_t output;

        if (in.empty())
        {
            uint8_t buf[6] = {0x02, 0x00, 0x00, 0x00, 0xff, 0xff};
            out.append((char *)(buf), 6);
            return true;
        }

        _deflateState.avail_in = (uInt) in.size();
        _deflateState.next_in = (Bytef*) in.data();

        do
        {
            // Output to local buffer
            _deflateState.avail_out = (uInt) _compressBufferSize;
            _deflateState.next_out = _compressBuffer.get();

            deflate(&_deflateState, _flush);

            output = _compressBufferSize - _deflateState.avail_out;

            out.append((char *)(_compressBuffer.get()),output);
        } while (_deflateState.avail_out == 0);

        if (endsWith(out, kEmptyUncompressedBlock))
        {
            out.resize(out.size() - 4);
        }

        return true;
    }

    //
    // Decompressor
    //
    WebSocketPerMessageDeflateDecompressor::WebSocketPerMessageDeflateDecompressor()
      : _compressBufferSize(kBufferSize)
    {
        _inflateState.zalloc = Z_NULL;
        _inflateState.zfree = Z_NULL;
        _inflateState.opaque = Z_NULL;
        _inflateState.avail_in = 0;
        _inflateState.next_in = Z_NULL;
    }

    WebSocketPerMessageDeflateDecompressor::~WebSocketPerMessageDeflateDecompressor()
    {
        inflateEnd(&_inflateState);
    }

    bool WebSocketPerMessageDeflateDecompressor::init(uint8_t inflateBits,
                                                      bool clientNoContextTakeOver)
    {
        int ret = inflateInit2(
            &_inflateState,
            -1*inflateBits
        );

        if (ret != Z_OK) return false;

        _compressBuffer.reset(new unsigned char[_compressBufferSize]);
        _flush = (clientNoContextTakeOver)
                 ? Z_FULL_FLUSH
                 : Z_SYNC_FLUSH;

        return true;
    }

    bool WebSocketPerMessageDeflateDecompressor::decompress(const std::string& in,
                                                            std::string& out)
    {
        //
        // 7.2.2.  Decompression
        // 
        //    An endpoint uses the following algorithm to decompress a message.
        // 
        //    1.  Append 4 octets of 0x00 0x00 0xff 0xff to the tail end of the
        //        payload of the message.
        // 
        //    2.  Decompress the resulting data using DEFLATE.
        // 
        std::string inFixed(in);
        inFixed += kEmptyUncompressedBlock;

        _inflateState.avail_in = (uInt) inFixed.size();
        _inflateState.next_in = (unsigned char *)(const_cast<char *>(inFixed.data()));

        do
        {
            _inflateState.avail_out = (uInt) _compressBufferSize;
            _inflateState.next_out = _compressBuffer.get();

            int ret = inflate(&_inflateState, Z_SYNC_FLUSH);

            if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
            {
                return false; // zlib error
            }

            out.append(
                reinterpret_cast<char *>(_compressBuffer.get()),
                _compressBufferSize - _inflateState.avail_out
            );
        } while (_inflateState.avail_out == 0);

        return true;
    }

    WebSocketPerMessageDeflate::WebSocketPerMessageDeflate()
    {
        _compressor.reset(new WebSocketPerMessageDeflateCompressor());
        _decompressor.reset(new WebSocketPerMessageDeflateDecompressor());
    }

    WebSocketPerMessageDeflate::~WebSocketPerMessageDeflate()
    {
        _compressor.reset();
        _decompressor.reset();
    }

    bool WebSocketPerMessageDeflate::init(const WebSocketPerMessageDeflateOptions& perMessageDeflateOptions)
    {
        bool clientNoContextTakeover = 
            perMessageDeflateOptions.getClientNoContextTakeover();

        uint8_t deflateBits = perMessageDeflateOptions.getClientMaxWindowBits();
        uint8_t inflateBits = perMessageDeflateOptions.getServerMaxWindowBits();

        return _compressor->init(deflateBits, clientNoContextTakeover) && 
               _decompressor->init(inflateBits, clientNoContextTakeover);
    }

    bool WebSocketPerMessageDeflate::compress(const std::string& in,
                                              std::string& out)
    {
        return _compressor->compress(in, out);
    }

    bool WebSocketPerMessageDeflate::decompress(const std::string& in,
                                                std::string &out)
    {
        return _decompressor->decompress(in, out);
    }

}
