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

#include <folly/io/async/AsyncTransport.h>
#include <folly/futures/Future.h>
#include <folly/ExceptionWrapper.h>

namespace wangle {

class PipelineBase;

// HandlerContext定义(集inbound和outbound类型于一身)
// 以fire开始的方法都是Context中的事件方法
template <class In, class Out>
class HandlerContext {
public:
  virtual ~HandlerContext() = default;

  // inbound类型事件接口
  virtual void fireRead(In msg) = 0;
  virtual void fireReadEOF() = 0;
  virtual void fireReadException(folly::exception_wrapper e) = 0;
  virtual void fireTransportActive() = 0;
  virtual void fireTransportInactive() = 0;

  // outbound类型事件接口
  virtual folly::Future<folly::Unit> fireWrite(Out msg) = 0;
  virtual folly::Future<folly::Unit> fireWriteException(
    folly::exception_wrapper e) = 0;
  virtual folly::Future<folly::Unit> fireClose() = 0;


  virtual PipelineBase* getPipeline() = 0;
  virtual std::shared_ptr<PipelineBase> getPipelineShared() = 0;
  std::shared_ptr<folly::AsyncTransport> getTransport() {
    return getPipeline()->getTransport();
  }

  virtual void setWriteFlags(folly::WriteFlags flags) = 0;
  virtual folly::WriteFlags getWriteFlags() = 0;

  virtual void setReadBufferSettings(
    uint64_t minAvailable,
    uint64_t allocationSize) = 0;
  virtual std::pair<uint64_t, uint64_t> getReadBufferSettings() = 0;

  /* TODO
  template <class H>
  virtual void addHandlerBefore(H&&) {}
  template <class H>
  virtual void addHandlerAfter(H&&) {}
  template <class H>
  virtual void replaceHandler(H&&) {}
  virtual void removeHandler() {}
  */
};


// inbound 类型的InboundHandlerContext
template <class In>
class InboundHandlerContext {
public:
  virtual ~InboundHandlerContext() = default;

  virtual void fireRead(In msg) = 0;
  virtual void fireReadEOF() = 0;
  virtual void fireReadException(folly::exception_wrapper e) = 0;
  virtual void fireTransportActive() = 0;
  virtual void fireTransportInactive() = 0;

  virtual PipelineBase* getPipeline() = 0;
  virtual std::shared_ptr<PipelineBase> getPipelineShared() = 0;
  std::shared_ptr<folly::AsyncTransport> getTransport() {
    return getPipeline()->getTransport();
  }

  // TODO Need get/set writeFlags, readBufferSettings? Probably not.
  // Do we even really need them stored in the pipeline at all?
  // Could just always delegate to the socket impl
};


// outbound 类型的OutboundHandlerContext
template <class Out>
class OutboundHandlerContext {
public:
  virtual ~OutboundHandlerContext() = default;

  virtual folly::Future<folly::Unit> fireWrite(Out msg) = 0;
  virtual folly::Future<folly::Unit> fireWriteException(
    folly::exception_wrapper e) = 0;
  virtual folly::Future<folly::Unit> fireClose() = 0;

  virtual PipelineBase* getPipeline() = 0;
  virtual std::shared_ptr<PipelineBase> getPipelineShared() = 0;
  std::shared_ptr<folly::AsyncTransport> getTransport() {
    return getPipeline()->getTransport();
  }
};

// #include <windows.h> has blessed us with #define IN & OUT, typically mapped
// to nothing, so letting the preprocessor delete each of these symbols, leading
// to interesting compiler errors around HandlerDir.
#ifdef IN
#  undef IN
#endif
#ifdef OUT
#  undef OUT
#endif

enum class HandlerDir {
  IN,
  OUT,
  BOTH
};

} // namespace wangle

#include <wangle/channel/HandlerContext-inl.h>
