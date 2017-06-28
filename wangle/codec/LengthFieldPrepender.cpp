/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <wangle/codec/LengthFieldPrepender.h>

using folly::Future;
using folly::Unit;
using folly::IOBuf;

namespace wangle {

LengthFieldPrepender::LengthFieldPrepender(
    int lengthFieldLength,
    int lengthAdjustment,
    bool lengthIncludesLengthField,
    bool networkByteOrder)
    : lengthFieldLength_(lengthFieldLength)
    , lengthAdjustment_(lengthAdjustment)
    , lengthIncludesLengthField_(lengthIncludesLengthField)
    , networkByteOrder_(networkByteOrder) {
    CHECK(lengthFieldLength == 1 ||
          lengthFieldLength == 2 ||
          lengthFieldLength == 4 ||
          lengthFieldLength == 8 );
  }

Future<Unit> LengthFieldPrepender::write(Context* ctx, std::unique_ptr<IOBuf> buf) {
  // 总长度为本次要写的数据包的长度加上调整值
  int length = lengthAdjustment_ + buf->computeChainDataLength();
  // 如果协议长度包含长度字段本身
  if (lengthIncludesLengthField_) {
    // 那么总长度还需要加上长度字段占用的字节数
    length += lengthFieldLength_;
  }

  if (length < 0) {
    throw std::runtime_error("Length field < 0");
  }
  // 发送缓冲区
  auto len = IOBuf::create(lengthFieldLength_);
  // 多开辟lengthFieldLength_
  len->append(lengthFieldLength_);
  // 接收缓冲区的游标（处于最开始位置），便于按字节操作
  folly::io::RWPrivateCursor c(len.get());

  // 散转长度字段占用的字节数，最多八个字节
  switch (lengthFieldLength_) {
    case 1: {
      if (length >= 256) {
        throw std::runtime_error("length does not fit byte");
      }
      // 如果按照网络字节序
      if (networkByteOrder_) {
        // 大端形式写
        c.writeBE((uint8_t)length);
      } else {
        // 小端形式写
        c.writeLE((uint8_t)length);
      }
      break;
    }
    case 2: {
      if (length >= 65536) {
        throw std::runtime_error("length does not fit byte");
      }
      if (networkByteOrder_) {
        c.writeBE((uint16_t)length);
      } else {
        c.writeLE((uint16_t)length);
      }
      break;
    }
    case 4: {
      if (networkByteOrder_) {
        c.writeBE((uint32_t)length);
      } else {
        c.writeLE((uint32_t)length);
      }
      break;
    }
    case 8: {
      if (networkByteOrder_) {
        c.writeBE((uint64_t)length);
      } else {
        c.writeLE((uint64_t)length);
      }
      break;
    }
    default: {
      throw std::runtime_error("Invalid lengthFieldLength");
    }
  }

  // 把buf合并到len中
  len->prependChain(std::move(buf));
  return ctx->fireWrite(std::move(len));
}


} // namespace wangle
