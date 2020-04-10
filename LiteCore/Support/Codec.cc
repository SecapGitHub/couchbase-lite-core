//
// Codec.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


// For zlib API documentation, see: https://zlib.net/manual.html


#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "Endian.hh"
#include <algorithm>
#include <mutex>

namespace litecore { namespace blip {
    using namespace fleece;


    // "The windowBits parameter is the base two logarithm of the window size (the size of the
    // history buffer)." 15 is the max, and the suggested default value.
    static constexpr int kZlibWindowSize = 15;

    // True to use raw DEFLATE format, false to add the zlib header & checksum
    static constexpr bool kZlibRawDeflate = true;

    // "The memLevel parameter specifies how much memory should be allocated for the internal
    // compression state." Default is 8; we bump it to 9, which uses 256KB.
    static constexpr int kZlibDeflateMemLevel = 9;


    LogDomain Zip("Zip", LogLevel::Warning);


    Codec::Codec()
    :Logging(Zip)
    ,_checksum((uint32_t)crc32(0, nullptr, 0))    // the required initial value
    { }


    void Codec::addToChecksum(slice data) {
        _checksum = (uint32_t)crc32(_checksum, (const Bytef*)data.buf, (int)data.size);
    }

    void Codec::writeChecksum(slice &output) const {
        uint32_t chk = endian::enc32(_checksum);
        Assert(output.writeFrom(slice(&chk, sizeof(chk))));
    }


    void Codec::readAndVerifyChecksum(slice &input) const {
        if (input.size < kChecksumSize)
            error::_throw(error::CorruptData, "BLIP message ends before checksum");
        uint32_t chk;
        static_assert(kChecksumSize == sizeof(chk), "kChecksumSize is wrong");
        input.readInto(slice(&chk, sizeof(chk)));
        chk = endian::dec32(chk);
        if (chk != _checksum)
            error::_throw(error::CorruptData, "BLIP message invalid checksum");
    }


    // Uncompressed write: just copies input bytes to output (updating checksum)
    void Codec::_writeRaw(slice &input, slice &output) {
        logInfo("Copying %zu bytes into %zu-byte buf (no compression)", input.size, output.size);
        Assert(output.size > 0);
        size_t count = std::min(input.size, output.size);
        addToChecksum({input.buf, count});
        memcpy((void*)output.buf, input.buf, count);
        input.moveStart(count);
        output.moveStart(count);
    }


    void ZlibCodec::check(int ret) const {
        if (ret < 0 && ret != Z_BUF_ERROR)
            error::_throw(error::CorruptData, "zlib error %d: %s",
                          ret, (_z.msg ? _z.msg : "???"));
    }


    void ZlibCodec::_write(const char *operation,
                           slice &input, slice &output,
                           Mode mode,
                           size_t maxInput)
    {
        _z.next_in = (Bytef*)input.buf;
        auto inSize = _z.avail_in = (unsigned)std::min(input.size, maxInput);
        _z.next_out = (Bytef*)output.buf;
        auto outSize = _z.avail_out = (unsigned)output.size;
        Assert(outSize > 0);
        Assert(mode > Mode::Raw);
        int result = _flate(&_z, (int)mode);
        logInfo("    %s(in %u, out %u, mode %d)-> %d; read %ld bytes, wrote %ld bytes",
            operation, inSize, outSize, (int)mode, result,
            (long)(_z.next_in - (uint8_t*)input.buf),
            (long)(_z.next_out - (uint8_t*)output.buf));
        if (!kZlibRawDeflate)
            _checksum = (uint32_t)_z.adler;
        input.setStart(_z.next_in);
        output.setStart(_z.next_out);
        check(result);
    }


#pragma mark - DEFLATER:


    Deflater::Deflater(CompressionLevel level)
    :ZlibCodec(::deflate)
    {
        check(::deflateInit2(&_z,
                             level,
                             Z_DEFLATED,
                             kZlibWindowSize * (kZlibRawDeflate ? -1 : 1),
                             kZlibDeflateMemLevel,
                             Z_DEFAULT_STRATEGY));
    }


    Deflater::~Deflater() {
        ::deflateEnd(&_z);
    }


    void Deflater::write(slice &input, slice &output, Mode mode) {
        if (mode == Mode::Raw)
            return _writeRaw(input, output);

        slice origInput = input;
        size_t origOutputSize = output.size;
        logInfo("Compressing %zu bytes into %zu-byte buf", input.size, origOutputSize);

        switch (mode) {
            case Mode::NoFlush:     _write("deflate", input, output, mode); break;
            case Mode::SyncFlush:   _writeAndFlush(input, output); break;
            default:                error::_throw(error::InvalidParameter);
        }

        if (kZlibRawDeflate)
            addToChecksum({origInput.buf, input.buf});

        logInfo("    compressed %zu bytes to %zu (%.0f%%), %u unflushed",
            (origInput.size-input.size), (origOutputSize-output.size),
            (origOutputSize-output.size) * 100.0 / (origInput.size-input.size),
            unflushedBytes());
    }


    void Deflater::_writeAndFlush(slice &input, slice &output) {
        // If we try to write all of the input, and there isn't room in the output, the zlib
        // codec might end up with buffered data that hasn't been output yet (even though we
        // told it to flush.) To work around this, write the data gradually and stop before
        // the output fills up.
        static constexpr size_t kHeadroomForFlush = 12;
        static constexpr size_t kStopAtOutputSize = 100;

        Mode curMode = Mode::PartialFlush;
        while (input.size > 0) {
            if (output.size >= deflateBound(&_z, (unsigned)input.size)) {
                // Entire input is guaranteed to fit, so write it & flush:
                curMode = Mode::SyncFlush;
                _write("deflate", input, output, Mode::SyncFlush);
            } else {
                // Limit input size to what we know can be compressed into output.
                // Don't flush, because we may try to write again if there's still room.
                _write("deflate", input, output, curMode, output.size - kHeadroomForFlush);
            }
            if (output.size <= kStopAtOutputSize)
                break;
        }

        if (curMode != Mode::SyncFlush) {
            // Flush if we haven't yet (consuming no input):
            _write("deflate", input, output, Mode::SyncFlush, 0);
        }
    }


    unsigned Deflater::unflushedBytes() const {
#ifdef __APPLE__
        // zlib's deflatePending() is only available in iOS 10+ / macOS 10.12+,
        // even though <zlib.h> claims it's available in iOS 8 / macOS 10.10. (#471)
        if (__builtin_available(iOS 10, macOS 10.12, *)) {
#endif
            unsigned bytes;
            int bits;
            check(deflatePending(&_z, &bytes, &bits));
            return bytes + (bits > 0);
#ifdef __APPLE__
        } else {
            return 0;
        }
#endif
    }


#pragma mark - INFLATER:


    Inflater::Inflater()
    :ZlibCodec(::inflate)
    {
        check(::inflateInit2(&_z, kZlibRawDeflate ? (-kZlibWindowSize) : (kZlibWindowSize + 32)));
    }


    Inflater::~Inflater() {
        ::inflateEnd(&_z);
    }


    void Inflater::write(slice &input, slice &output, Mode mode) {
        if (mode == Mode::Raw)
            return _writeRaw(input, output);

        logInfo("Decompressing %zu bytes into %zu-byte buf", input.size, output.size);
        auto outStart = (uint8_t*)output.buf;
        _write("inflate", input, output, mode);
        if (kZlibRawDeflate)
            addToChecksum({outStart, output.buf});

        logDebug("    decompressed %ld bytes: %.*s",
                   (long)((uint8_t*)output.buf - outStart),
                   (int)((uint8_t*)output.buf - outStart), outStart);
    }

} }
