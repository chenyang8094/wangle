/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <wangle/codec/LengthFieldBasedFrameDecoder.h>

using folly::IOBuf;
using folly::IOBufQueue;

namespace wangle {

LengthFieldBasedFrameDecoder::LengthFieldBasedFrameDecoder(
  uint32_t lengthFieldLength,
  uint32_t maxFrameLength,
  uint32_t lengthFieldOffset,
  int32_t lengthAdjustment,
  uint32_t initialBytesToStrip,
  bool networkByteOrder)
    : lengthFieldLength_(lengthFieldLength)
    , maxFrameLength_(maxFrameLength)
    , lengthFieldOffset_(lengthFieldOffset)
    , lengthAdjustment_(lengthAdjustment)
    , initialBytesToStrip_(initialBytesToStrip)
    , networkByteOrder_(networkByteOrder)
    , lengthFieldEndOffset_(lengthFieldOffset + lengthFieldLength) {
  CHECK(maxFrameLength > 0);
  CHECK(lengthFieldOffset <= maxFrameLength - lengthFieldLength);
}

bool LengthFieldBasedFrameDecoder::decode(Context* ctx,
                                          IOBufQueue& buf,
                                          std::unique_ptr<IOBuf>& result,
                                          size_t&) {
  // 如果读到的数据包长度还不足以读取长度字段，那么直接返回解码失败
  if (buf.chainLength() < lengthFieldEndOffset_) {
    return false;
  }

  // 解码获取未经调整的帧长度
  uint64_t frameLength = getUnadjustedFrameLength(
    buf, lengthFieldOffset_, lengthFieldLength_, networkByteOrder_);

  // 对帧长度进行调整
  frameLength += lengthAdjustment_ + lengthFieldEndOffset_;

  // 如果帧长度小于长度字段结束位置的偏移
  if (frameLength < lengthFieldEndOffset_) {
    buf.trimStart(lengthFieldEndOffset_);
    ctx->fireReadException(folly::make_exception_wrapper<std::runtime_error>(
                             "Frame too small"));
    return false;
  }

  // 如果帧长度大于最大长度
  if (frameLength > maxFrameLength_) {
    buf.trimStart(frameLength);
    ctx->fireReadException(folly::make_exception_wrapper<std::runtime_error>(
                             "Frame larger than " +
                             folly::to<std::string>(maxFrameLength_)));
    return false;
  }

  // 如果已经读到的数据包长度小于帧长度
  if (buf.chainLength() < frameLength) {
    // 返回解码失败
    return false;
  }

  // 如果要初始化丢弃的字节数比帧长度还要大
  if (initialBytesToStrip_ > frameLength) {
    buf.trimStart(frameLength);
    ctx->fireReadException(folly::make_exception_wrapper<std::runtime_error>(
                             "InitialBytesToSkip larger than frame"));
    return false;
  }
  // 丢弃字节数
  buf.trimStart(initialBytesToStrip_);
  // 计算最终的实际长度
  int actualFrameLength = frameLength - initialBytesToStrip_;
  result = buf.split(actualFrameLength);
  return true;
}

uint64_t LengthFieldBasedFrameDecoder::getUnadjustedFrameLength(
  IOBufQueue& buf, int offset, int length, bool networkByteOrder) {
  folly::io::Cursor c(buf.front());
  uint64_t frameLength;

  c.skip(offset);

  switch(length) {
    case 1:{
      if (networkByteOrder) {
        frameLength = c.readBE<uint8_t>();
      } else {
        frameLength = c.readLE<uint8_t>();
      }
      break;
    }
    case 2:{
      if (networkByteOrder) {
        frameLength = c.readBE<uint16_t>();
      } else {
        frameLength = c.readLE<uint16_t>();
      }
      break;
    }
    case 4:{
      if (networkByteOrder) {
        frameLength = c.readBE<uint32_t>();
      } else {
        frameLength = c.readLE<uint32_t>();
      }
      break;
    }
    case 8:{
      if (networkByteOrder) {
        frameLength = c.readBE<uint64_t>();
      } else {
        frameLength = c.readLE<uint64_t>();
      }
      break;
    }
  }

  return frameLength;
}


} // namespace wangle
