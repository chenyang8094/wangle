/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <folly/futures/Future.h>
#include <wangle/channel/Pipeline.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

namespace wangle {

// Handler基类,类型参数为Context类型
template <class Context>
class HandlerBase {
public:
  virtual ~HandlerBase() = default;

  virtual void attachPipeline(Context* /*ctx*/) {}
  virtual void detachPipeline(Context* /*ctx*/) {}

  // 获取绑定的Context
  Context* getContext() {
    if (attachCount_ != 1) {
      return nullptr;
    }
    CHECK(ctx_);
    return ctx_;
  }

private:
  friend PipelineContext;    // 设置PipelineContext为友元类，便于PipelineContext操作自己
  uint64_t attachCount_{0};  // 绑定计数，同一个handler可以被同时绑定到不同的pipeline中
  Context* ctx_{nullptr};    // 该Handler绑定的Context
};

// Rin:输入给read的消息类型，Rout:为从read返回时调用Context的fireRead时传递的消息类型，一般情况下与Rin一致
// Win:为输入给write的消息类型，Wout：为从write返回值调用Context的fireWrite时传递的消息类型，一般情况下与Win一致
template <class Rin, class Rout = Rin, class Win = Rout, class Wout = Rin>
class Handler : public HandlerBase<HandlerContext<Rout, Wout>> {
public:
  static const HandlerDir dir = HandlerDir::BOTH; // 方向为双向

  typedef Rin rin;
  typedef Rout rout;
  typedef Win win;
  typedef Wout wout;
  typedef HandlerContext<Rout, Wout> Context;  // 声明该HandlerContext类型
  virtual ~Handler() = default;

  // inbound类型事件
  virtual void read(Context* ctx, Rin msg) = 0;
  virtual void readEOF(Context* ctx) {
    ctx->fireReadEOF();
  }
  virtual void readException(Context* ctx, folly::exception_wrapper e) {
    ctx->fireReadException(std::move(e));
  }
  virtual void transportActive(Context* ctx) {
    ctx->fireTransportActive();
  }
  virtual void transportInactive(Context* ctx) {
    ctx->fireTransportInactive();
  }

  // outbound类型事件
  virtual folly::Future<folly::Unit> write(Context* ctx, Win msg) = 0;
  virtual folly::Future<folly::Unit> writeException(Context* ctx,
      folly::exception_wrapper e) {
    return ctx->fireWriteException(std::move(e));
  }
  virtual folly::Future<folly::Unit> close(Context* ctx) {
    return ctx->fireClose();
  }

  /*
  // Other sorts of things we might want, all shamelessly stolen from Netty
  // inbound
  virtual void exceptionCaught(
      HandlerContext* ctx,
      folly::exception_wrapper e) {}
  virtual void channelRegistered(HandlerContext* ctx) {}
  virtual void channelUnregistered(HandlerContext* ctx) {}
  virtual void channelReadComplete(HandlerContext* ctx) {}
  virtual void userEventTriggered(HandlerContext* ctx, void* evt) {}
  virtual void channelWritabilityChanged(HandlerContext* ctx) {}

  // outbound
  virtual folly::Future<folly::Unit> bind(
      HandlerContext* ctx,
      SocketAddress localAddress) {}
  virtual folly::Future<folly::Unit> connect(
          HandlerContext* ctx,
          SocketAddress remoteAddress, SocketAddress localAddress) {}
  virtual folly::Future<folly::Unit> disconnect(HandlerContext* ctx) {}
  virtual folly::Future<folly::Unit> deregister(HandlerContext* ctx) {}
  virtual folly::Future<folly::Unit> read(HandlerContext* ctx) {}
  virtual void flush(HandlerContext* ctx) {}
  */
};


// inbound类型的Handler （默认情况下读入和读出的类型是一致）
template <class Rin, class Rout = Rin>
class InboundHandler : public HandlerBase<InboundHandlerContext<Rout>> {
public:
  static const HandlerDir dir = HandlerDir::IN;  // 方向为输入

  typedef Rin rin;
  typedef Rout rout;
  typedef folly::Unit win;
  typedef folly::Unit wout;
  typedef InboundHandlerContext<Rout> Context; // 声明inbound类型的InboundHandlerContext
  virtual ~InboundHandler() = default;

  // 纯虚函数。由子类实现
  virtual void read(Context* ctx, Rin msg) = 0;
  // 下面的默认实现都是事件的透传
  virtual void readEOF(Context* ctx) {
    ctx->fireReadEOF();
  }
  virtual void readException(Context* ctx, folly::exception_wrapper e) {
    ctx->fireReadException(std::move(e));// std::move
  }
  virtual void transportActive(Context* ctx) {
    ctx->fireTransportActive();
  }
  virtual void transportInactive(Context* ctx) {
    ctx->fireTransportInactive();
  }
};

// outbound类型的Handler (默认写入类型和写出类型一致，如果不一致就会产生很多的转换)
template <class Win, class Wout = Win>
class OutboundHandler : public HandlerBase<OutboundHandlerContext<Wout>> {
public:
  static const HandlerDir dir = HandlerDir::OUT; // 方向为输出

  typedef folly::Unit rin;
  typedef folly::Unit rout;
  typedef Win win;
  typedef Wout wout;
  typedef OutboundHandlerContext<Wout> Context;
  virtual ~OutboundHandler() = default;

  // 纯虚函数。由子类实现
  virtual folly::Future<folly::Unit> write(Context* ctx, Win msg) = 0;
  // 下面的默认实现都是事件的透传
  virtual folly::Future<folly::Unit> writeException(
    Context* ctx, folly::exception_wrapper e) {
    return ctx->fireWriteException(std::move(e));
  }
  virtual folly::Future<folly::Unit> close(Context* ctx) {
    return ctx->fireClose();
  }
};


// Handler适配器
template <class R, class W = R>
class HandlerAdapter : public Handler<R, R, W, W> {
public:
  typedef typename Handler<R, R, W, W>::Context Context;

  // 将read事件直接进行透传
  void read(Context* ctx, R msg) override {
    ctx->fireRead(std::forward<R>(msg));
  }

  // 将write事件直接进行透传
  folly::Future<folly::Unit> write(Context* ctx, W msg) override {
    return ctx->fireWrite(std::forward<W>(msg));
  }
};

// BytesToBytesHandler
typedef HandlerAdapter<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>
    BytesToBytesHandler;

// InboundBytesToBytesHandler
typedef InboundHandler<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>
    InboundBytesToBytesHandler;

// OutboundBytesToBytesHandler
typedef OutboundHandler<std::unique_ptr<folly::IOBuf>>
    OutboundBytesToBytesHandler;

} // namespace wangle
