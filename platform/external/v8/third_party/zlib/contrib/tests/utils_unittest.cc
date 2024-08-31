// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the Chromium source repository LICENSE file.

#include "infcover.h"

#include <cstddef>
#include <vector>

#include "compression_utils_portable.h"
#include "gtest.h"
#include "zlib.h"

void TestPayloads(size_t input_size, zlib_internal::WrapperType type) {
  std::vector<unsigned char> input;
  input.reserve(input_size);
  for (size_t i = 1; i <= input_size; ++i)
    input.push_back(i & 0xff);

  // If it is big enough for GZIP, will work for other wrappers.
  std::vector<unsigned char> compressed(
      zlib_internal::GzipExpectedCompressedSize(input.size()));
  std::vector<unsigned char> decompressed(input.size());

  // Libcores's java/util/zip/Deflater default settings: ZLIB,
  // DEFAULT_COMPRESSION and DEFAULT_STRATEGY.
  unsigned long compressed_size = static_cast<unsigned long>(compressed.size());
  int result = zlib_internal::CompressHelper(
      type, compressed.data(), &compressed_size, input.data(), input.size(),
      Z_DEFAULT_COMPRESSION, nullptr, nullptr);
  ASSERT_EQ(result, Z_OK);

  unsigned long decompressed_size =
      static_cast<unsigned long>(decompressed.size());
  result = zlib_internal::UncompressHelper(type, decompressed.data(),
                                           &decompressed_size,
                                           compressed.data(), compressed_size);
  ASSERT_EQ(result, Z_OK);
  EXPECT_EQ(input, decompressed);
}

TEST(ZlibTest, ZlibWrapper) {
  // Minimal ZLIB wrapped short stream size is about 8 bytes.
  for (size_t i = 1; i < 1024; ++i)
    TestPayloads(i, zlib_internal::WrapperType::ZLIB);
}

TEST(ZlibTest, GzipWrapper) {
  // GZIP should be 12 bytes bigger than ZLIB wrapper.
  for (size_t i = 1; i < 1024; ++i)
    TestPayloads(i, zlib_internal::WrapperType::GZIP);
}

TEST(ZlibTest, RawWrapper) {
  // RAW has no wrapper (V8 Blobs is a known user), size
  // should be payload_size + 2 for short payloads.
  for (size_t i = 1; i < 1024; ++i)
    TestPayloads(i, zlib_internal::WrapperType::ZRAW);
}

TEST(ZlibTest, InflateCover) {
  cover_support();
  cover_wrap();
  cover_back();
  cover_inflate();
  // TODO(cavalcantii): enable this last test.
  // cover_trees();
  cover_fast();
}

TEST(ZlibTest, DeflateStored) {
  const int no_compression = 0;
  const zlib_internal::WrapperType type = zlib_internal::WrapperType::GZIP;
  std::vector<unsigned char> input(1 << 10, 42);
  std::vector<unsigned char> compressed(
      zlib_internal::GzipExpectedCompressedSize(input.size()));
  std::vector<unsigned char> decompressed(input.size());
  unsigned long compressed_size = static_cast<unsigned long>(compressed.size());
  int result = zlib_internal::CompressHelper(
      type, compressed.data(), &compressed_size, input.data(), input.size(),
      no_compression, nullptr, nullptr);
  ASSERT_EQ(result, Z_OK);

  unsigned long decompressed_size =
      static_cast<unsigned long>(decompressed.size());
  result = zlib_internal::UncompressHelper(type, decompressed.data(),
                                           &decompressed_size,
                                           compressed.data(), compressed_size);
  ASSERT_EQ(result, Z_OK);
  EXPECT_EQ(input, decompressed);
}

TEST(ZlibTest, StreamingInflate) {
  uint8_t comp_buf[4096], decomp_buf[4096];
  z_stream comp_strm, decomp_strm;
  int ret;

  std::vector<uint8_t> src;
  for (size_t i = 0; i < 1000; i++) {
    for (size_t j = 0; j < 40; j++) {
      src.push_back(j);
    }
  }

  // Deflate src into comp_buf.
  comp_strm.zalloc = Z_NULL;
  comp_strm.zfree = Z_NULL;
  comp_strm.opaque = Z_NULL;
  ret = deflateInit(&comp_strm, Z_BEST_COMPRESSION);
  ASSERT_EQ(ret, Z_OK);
  comp_strm.next_out = comp_buf;
  comp_strm.avail_out = sizeof(comp_buf);
  comp_strm.next_in = src.data();
  comp_strm.avail_in = src.size();
  ret = deflate(&comp_strm, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  size_t comp_sz = sizeof(comp_buf) - comp_strm.avail_out;

  // Inflate comp_buf one 4096-byte buffer at a time.
  decomp_strm.zalloc = Z_NULL;
  decomp_strm.zfree = Z_NULL;
  decomp_strm.opaque = Z_NULL;
  ret = inflateInit(&decomp_strm);
  ASSERT_EQ(ret, Z_OK);
  decomp_strm.next_in = comp_buf;
  decomp_strm.avail_in = comp_sz;

  while (decomp_strm.avail_in > 0) {
    decomp_strm.next_out = decomp_buf;
    decomp_strm.avail_out = sizeof(decomp_buf);
    ret = inflate(&decomp_strm, Z_FINISH);
    ASSERT_TRUE(ret == Z_OK || ret == Z_STREAM_END || ret == Z_BUF_ERROR);

    // Verify the output bytes.
    size_t num_out = sizeof(decomp_buf) - decomp_strm.avail_out;
    for (size_t i = 0; i < num_out; i++) {
      EXPECT_EQ(decomp_buf[i], src[decomp_strm.total_out - num_out + i]);
    }
  }

  // Cleanup memory (i.e. makes ASAN bot happy).
  ret = deflateEnd(&comp_strm);
  EXPECT_EQ(ret, Z_OK);
  ret = inflateEnd(&decomp_strm);
  EXPECT_EQ(ret, Z_OK);
}

TEST(ZlibTest, CRCHashBitsCollision) {
  // The CRC32c of the hex sequences 2a,14,14,14 and 2a,14,db,14 have the same
  // lower 9 bits. Since longest_match doesn't check match[2], a bad match could
  // be chosen when the number of hash bits is <= 9. For this reason, the number
  // of hash bits must be set higher, regardless of the memlevel parameter, when
  // using CRC32c hashing for string matching. See https://crbug.com/1113596

  std::vector<uint8_t> src = {
      // Random byte; zlib doesn't match at offset 0.
      123,

      // This will look like 5-byte match.
      0x2a,
      0x14,
      0xdb,
      0x14,
      0x15,

      // Offer a 4-byte match to bump the next expected match length to 5.
      0x2a,
      0x14,
      0x14,
      0x14,

      0x2a,
      0x14,
      0x14,
      0x14,
      0x15,
  };

  z_stream stream;
  stream.zalloc = nullptr;
  stream.zfree = nullptr;

  // Using a low memlevel to try to reduce the number of hash bits. Negative
  // windowbits means raw deflate, i.e. without the zlib header.
  int ret = deflateInit2(&stream, /*comp level*/ 2, /*method*/ Z_DEFLATED,
                         /*windowbits*/ -15, /*memlevel*/ 2,
                         /*strategy*/ Z_DEFAULT_STRATEGY);
  ASSERT_EQ(ret, Z_OK);
  std::vector<uint8_t> compressed(100, '\0');
  stream.next_out = compressed.data();
  stream.avail_out = compressed.size();
  stream.next_in = src.data();
  stream.avail_in = src.size();
  ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  compressed.resize(compressed.size() - stream.avail_out);
  deflateEnd(&stream);

  ret = inflateInit2(&stream, /*windowbits*/ -15);
  ASSERT_EQ(ret, Z_OK);
  std::vector<uint8_t> decompressed(src.size(), '\0');
  stream.next_in = compressed.data();
  stream.avail_in = compressed.size();
  stream.next_out = decompressed.data();
  stream.avail_out = decompressed.size();
  ret = inflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(0U, stream.avail_out);
  inflateEnd(&stream);

  EXPECT_EQ(src, decompressed);
}

TEST(ZlibTest, CRCHashAssert) {
  // The CRC32c of the hex sequences ff,ff,5e,6f and ff,ff,13,ff have the same
  // lower 15 bits. This means longest_match's assert that match[2] == scan[2]
  // won't hold. However, such hash collisions are only possible when one of the
  // other four bytes also mismatch. This tests that zlib's assert handles this
  // case.

  std::vector<uint8_t> src = {
      // Random byte; zlib doesn't match at offset 0.
      123,

      // This has the same hash as the last byte sequence, and the first two and
      // last two bytes match; though the third and the fourth don't.
      0xff,
      0xff,
      0x5e,
      0x6f,
      0x12,
      0x34,

      // Offer a 5-byte match to bump the next expected match length to 6
      // (because the two first and two last bytes need to match).
      0xff,
      0xff,
      0x13,
      0xff,
      0x12,

      0xff,
      0xff,
      0x13,
      0xff,
      0x12,
      0x34,
  };

  z_stream stream;
  stream.zalloc = nullptr;
  stream.zfree = nullptr;

  int ret = deflateInit2(&stream, /*comp level*/ 5, /*method*/ Z_DEFLATED,
                         /*windowbits*/ -15, /*memlevel*/ 8,
                         /*strategy*/ Z_DEFAULT_STRATEGY);
  ASSERT_EQ(ret, Z_OK);
  std::vector<uint8_t> compressed(100, '\0');
  stream.next_out = compressed.data();
  stream.avail_out = compressed.size();
  stream.next_in = src.data();
  stream.avail_in = src.size();
  ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  compressed.resize(compressed.size() - stream.avail_out);
  deflateEnd(&stream);

  ret = inflateInit2(&stream, /*windowbits*/ -15);
  ASSERT_EQ(ret, Z_OK);
  std::vector<uint8_t> decompressed(src.size(), '\0');
  stream.next_in = compressed.data();
  stream.avail_in = compressed.size();
  stream.next_out = decompressed.data();
  stream.avail_out = decompressed.size();
  ret = inflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(0U, stream.avail_out);
  inflateEnd(&stream);

  EXPECT_EQ(src, decompressed);
}

// Fuzzer generated.
static const uint8_t checkMatchCrashData[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x00,
    0x6e, 0x6e, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x39, 0x6e, 0x6e,
    0x00, 0x00, 0x00, 0x00, 0xf7, 0xff, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x6e,
    0x00, 0x00, 0x0a, 0x9a, 0x00, 0x00, 0x6e, 0x6e, 0x6e, 0x2a, 0x00, 0x00,
    0x00, 0xd5, 0xf0, 0x00, 0x81, 0x02, 0xf3, 0xfd, 0xff, 0xab, 0xf3, 0x6e,
    0x7e, 0x04, 0x5b, 0xf6, 0x2a, 0x2c, 0xf8, 0x00, 0x54, 0xf3, 0xa5, 0x0e,
    0xfd, 0x6e, 0xff, 0x00, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa4, 0x0b, 0xa5, 0x2a, 0x0d, 0x10, 0x01, 0x26, 0xf6, 0x04, 0x0e,
    0xff, 0x6e, 0x6e, 0x6e, 0x76, 0x00, 0x00, 0x87, 0x01, 0xfe, 0x0d, 0xb6,
    0x6e, 0x6e, 0xf7, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xfd, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0x9b,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a,
    0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x6e, 0xff, 0xff, 0x00,
    0x00, 0xd5, 0xf0, 0x00, 0xff, 0x40, 0x7e, 0x0b, 0xa5, 0x10, 0x67, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x40, 0x7e, 0x0b, 0xa5, 0x10, 0x67,
    0x7e, 0x32, 0x6e, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x40, 0x0b, 0xa5,
    0x10, 0x67, 0x01, 0xfe, 0x0d, 0xb6, 0x2a, 0x00, 0x00, 0x58, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6e, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3d, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xd6, 0x2d, 0x2d, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a,
    0x8a, 0x8a, 0x8a, 0x8a, 0x66, 0x8a, 0x8a, 0x8a, 0xee, 0x1d, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0xee, 0x0a, 0x00, 0x00, 0x00, 0x54, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xf3, 0x00, 0x00, 0xff, 0xff, 0x23, 0x7e, 0x00, 0x1e,
    0x00, 0x00, 0xd5, 0xf0, 0x00, 0xff, 0x40, 0x0b, 0xa5, 0x10, 0x67, 0x01,
    0xfe, 0x0d, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a,
    0x8a, 0x8a, 0x8a, 0x2d, 0x6e, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x0e,
    0xfb, 0x00, 0x10, 0x24, 0x00, 0x00, 0xfb, 0xff, 0x00, 0x00, 0xff, 0x1f,
    0xb3, 0x00, 0x04, 0x3d, 0x00, 0xee, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00,
    0x01, 0x45, 0x3d, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x11, 0x21, 0x00, 0x1e,
    0x00, 0x0c, 0xb3, 0xfe, 0x0e, 0xee, 0x02, 0x00, 0x1d, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x6e, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x6e, 0x00,
    0x00, 0x87, 0x00, 0x33, 0x38, 0x6e, 0x6e, 0x6e, 0x6e, 0x6e, 0x00, 0x00,
    0x00, 0x38, 0x00, 0x00, 0xff, 0xff, 0xff, 0x04, 0x3f, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0xff, 0x00, 0x31, 0x13, 0x13, 0x13,
    0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xab, 0x30, 0x83, 0x33,
    0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0xff, 0xff, 0x7d, 0xff, 0x00, 0x01,
    0x10, 0x0d, 0x2a, 0xa5, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x11,
    0x21, 0x00, 0xa5, 0x00, 0x68, 0x68, 0x68, 0x67, 0x00, 0x00, 0xff, 0xff,
    0x02, 0x00, 0x00, 0x68, 0x68, 0x68, 0x68, 0x00, 0x00, 0xfa, 0xff, 0xff,
    0x03, 0x01, 0xff, 0x02, 0x00, 0x00, 0x68, 0x68, 0x68, 0x68, 0x0a, 0x10,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0x06, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xfa, 0xff, 0xff, 0x08, 0xff, 0xff, 0xff, 0x00, 0x06, 0x04,
    0x00, 0xf8, 0xff, 0xff, 0x00, 0x01, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0x00, 0x06, 0x04, 0x6e,
    0x7e, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x00,
    0x00, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87, 0x6e, 0x6e, 0x6e,
    0x00, 0x01, 0x38, 0xd5, 0xf0, 0x00, 0x00, 0x2a, 0xfe, 0x04, 0x5b, 0x0d,
    0xfd, 0x6e, 0x92, 0x28, 0xf9, 0xfb, 0xff, 0x07, 0xd2, 0xd6, 0x2d, 0x2d,
    0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a, 0x8a,
    0x8a, 0x8a, 0xc2, 0x91, 0x00, 0x5b, 0xef, 0xde, 0xf2, 0x6e, 0x6e, 0xfd,
    0x0c, 0x02, 0x91, 0x62, 0x91, 0xfd, 0x6e, 0x6e, 0xd3, 0x06, 0x00, 0x00,
    0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00,
    0xd5, 0xf0, 0x00, 0xff, 0x00, 0x00, 0x31, 0x13, 0x13, 0x13, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x04, 0x00, 0x13, 0x0a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x6e, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x09, 0x00, 0x6a, 0x24, 0x26, 0x30, 0x01, 0x2e, 0x2a, 0xfe,
    0x04, 0x5b, 0x0d, 0xfd, 0x6e, 0x6e, 0xd7, 0x06, 0x6e, 0x6e, 0x6e, 0x00,
    0x00, 0xb1, 0xb1, 0xb1, 0xb1, 0x00, 0x00, 0x00, 0x6e, 0x5b, 0x00, 0x00,
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1e, 0x00, 0x00, 0x00, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6b, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x0b,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x24, 0x2a, 0x6e, 0x5c, 0x24,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xeb,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x05, 0x00, 0x00, 0x00, 0x5d, 0x10, 0x6e, 0x6e, 0xa5, 0x2f, 0x00, 0x00,
    0x95, 0x87, 0x00, 0x6e};

TEST(ZlibTest, CheckMatchCrash) {
  // See https://crbug.com/1113142.
  z_stream stream;
  stream.zalloc = nullptr;
  stream.zfree = nullptr;

  // Low windowbits to hit window sliding also with a relatively small input.
  int ret = deflateInit2(&stream, /*comp level*/ 5, /*method*/ Z_DEFLATED,
                         /*windowbits*/ -9, /*memlevel*/ 8,
                         /*strategy*/ Z_DEFAULT_STRATEGY);
  ASSERT_EQ(ret, Z_OK);

  uint8_t compressed[sizeof(checkMatchCrashData) * 2];
  stream.next_out = compressed;
  stream.avail_out = sizeof(compressed);

  for (size_t i = 0; i < sizeof(checkMatchCrashData); i++) {
    ASSERT_GT(stream.avail_out, 0U);
    stream.next_in = (uint8_t*)&checkMatchCrashData[i];
    stream.avail_in = 1;
    ret = deflate(&stream, Z_NO_FLUSH);
    ASSERT_EQ(ret, Z_OK);
  }

  stream.next_in = nullptr;
  stream.avail_in = 0;
  ASSERT_GT(stream.avail_out, 0U);
  ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  size_t compressed_sz = sizeof(compressed) - stream.avail_out;
  deflateEnd(&stream);

  uint8_t decompressed[sizeof(checkMatchCrashData)];
  ret = inflateInit2(&stream, -15);
  ASSERT_EQ(ret, Z_OK);
  stream.next_in = compressed;
  stream.avail_in = compressed_sz;
  stream.next_out = decompressed;
  stream.avail_out = sizeof(decompressed);
  ret = inflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  inflateEnd(&stream);
  ASSERT_EQ(
      memcmp(checkMatchCrashData, decompressed, sizeof(checkMatchCrashData)),
      0);
}

TEST(ZlibTest, DeflateRLEUninitUse) {
  // MSan would complain about use of uninitialized values in deflate_rle if the
  // window isn't zero-initialized. See crbug.com/1137613. Similar problems
  // exist in other places in zlib, e.g. longest_match (crbug.com/1144420) but
  // we don't have as nice test cases.

  int level = 9;
  int windowBits = 9;
  int memLevel = 8;
  int strategy = Z_RLE;
  const std::vector<uint8_t> src{
      0x31, 0x64, 0x38, 0x32, 0x30, 0x32, 0x30, 0x36, 0x65, 0x35, 0x38, 0x35,
      0x32, 0x61, 0x30, 0x36, 0x65, 0x35, 0x32, 0x66, 0x30, 0x34, 0x38, 0x37,
      0x61, 0x31, 0x38, 0x36, 0x37, 0x37, 0x31, 0x39, 0x0a, 0x65, 0x62, 0x00,
      0x9f, 0xff, 0xc6, 0xc6, 0xc6, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f, 0xff,
      0xc6, 0xc6, 0xc6, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f, 0xff, 0xc6, 0xc6,
      0xc6, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f, 0xff, 0xc6, 0xc6, 0xc6, 0x95,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0e, 0x0a, 0x54, 0x52,
      0x58, 0x56, 0xab, 0x26, 0x13, 0x53, 0x5a, 0xb5, 0x30, 0xbb, 0x96, 0x44,
      0x80, 0xe6, 0xc5, 0x0a, 0xd0, 0x47, 0x7a, 0xa0, 0x4e, 0xbe, 0x30, 0xdc,
      0xa1, 0x08, 0x54, 0xe1, 0x51, 0xd1, 0xea, 0xef, 0xdb, 0xa1, 0x2d, 0xb4,
      0xb9, 0x58, 0xb1, 0x2f, 0xf0, 0xae, 0xbc, 0x07, 0xd1, 0xba, 0x7f, 0x14,
      0xa4, 0xde, 0x99, 0x7f, 0x4d, 0x3e, 0x25, 0xd9, 0xef, 0xee, 0x4f, 0x38,
      0x7b, 0xaf, 0x3f, 0x6b, 0x53, 0x5a, 0xcb, 0x1f, 0x97, 0xb5, 0x43, 0xa3,
      0xe8, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f, 0xff, 0xc6, 0xc6, 0xc6, 0xff,
      0x09, 0x00, 0x62, 0x00, 0x9f, 0xff, 0xc6, 0xc6, 0xc6, 0xff, 0x09, 0x00,
      0x62, 0x00, 0x9f, 0xff, 0xc6, 0xc6, 0xc6, 0xff, 0x09, 0x00, 0x62, 0x00,
      0x9f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x3c,
      0x73, 0x70, 0x23, 0x87, 0xec, 0xf8, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xc1, 0x00, 0x00, 0x9f, 0xc6, 0xc6, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f,
      0xff, 0xc6, 0xc6, 0xc6, 0xff, 0x09, 0x00, 0x62, 0x00, 0x9f, 0xff, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
  };

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;

  // Compress the data one byte at a time to exercise the streaming code.
  int ret =
      deflateInit2(&stream, level, Z_DEFLATED, windowBits, memLevel, strategy);
  ASSERT_EQ(ret, Z_OK);
  std::vector<uint8_t> compressed(src.size() * 2 + 1000);
  stream.next_out = compressed.data();
  stream.avail_out = compressed.size();
  for (uint8_t b : src) {
    stream.next_in = &b;
    stream.avail_in = 1;
    ret = deflate(&stream, Z_NO_FLUSH);
    ASSERT_EQ(ret, Z_OK);
  }
  stream.next_in = Z_NULL;
  stream.avail_in = 0;
  ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);
  deflateEnd(&stream);
}
