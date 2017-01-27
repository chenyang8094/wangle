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

#include <boost/variant.hpp>
#include <folly/ExceptionWrapper.h>
#include <folly/Memory.h>
#include <folly/futures/Future.h>
#include <folly/Unit.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestruction.h>
#include <wangle/acceptor/SecureTransportType.h>
#include <wangle/acceptor/TransportInfo.h>
#include <wangle/channel/HandlerContext.h>

namespace wangle {

class PipelineBase;
class Acceptor;

// pipeline管理器
class PipelineManager {
 public:
  virtual ~PipelineManager() = default;
  virtual void deletePipeline(PipelineBase* pipeline) = 0;
  virtual void refreshTimeout() {};
};


// pipeline 基类
class PipelineBase : public std::enable_shared_from_this<PipelineBase> {
 public:
  virtual ~PipelineBase() = default;

  // 设置PipelineManager
  void setPipelineManager(PipelineManager* manager) {
    manager_ = manager;
  }

  // 获取PipelineManager
  PipelineManager* getPipelineManager() {
    return manager_;
  }
  
  // 删除pipeline
  void deletePipeline() {
    if (manager_) {
      // 会调用PipelineManager的相应的方法
      manager_->deletePipeline(this);
    }
  }

  // 设置一个异步AsyncTransport（为folly::AsyncSocket父类）
  void setTransport(std::shared_ptr<folly::AsyncTransport> transport) {
    transport_ = transport;
  }

  std::shared_ptr<folly::AsyncTransport> getTransport() {
    return transport_;
  }
  
  // 设置和获取write标识
  void setWriteFlags(folly::WriteFlags flags);
  folly::WriteFlags getWriteFlags();

  // 设置和获取read缓冲区
  void setReadBufferSettings(uint64_t minAvailable, uint64_t allocationSize);
  std::pair<uint64_t, uint64_t> getReadBufferSettings();
 
  // 设置和获取TransportInfo
  void setTransportInfo(std::shared_ptr<TransportInfo> tInfo);
  std::shared_ptr<TransportInfo> getTransportInfo();

  template <class H>
  PipelineBase& addBack(std::shared_ptr<H> handler);

  template <class H>
  PipelineBase& addBack(H&& handler);

  template <class H>
  PipelineBase& addBack(H* handler);

  template <class H>
  PipelineBase& addFront(std::shared_ptr<H> handler);

  template <class H>
  PipelineBase& addFront(H&& handler);

  template <class H>
  PipelineBase& addFront(H* handler);

  template <class H>
  PipelineBase& remove(H* handler);

  template <class H>
  PipelineBase& remove();

  PipelineBase& removeFront();

  PipelineBase& removeBack();

  template <class H>
  H* getHandler(int i);

  template <class H>
  H* getHandler();

  template <class H>
  typename ContextType<H>::type* getContext(int i);

  template <class H>
  typename ContextType<H>::type* getContext();

  // 如果handlers中的某一个拥有pipeline自身，那么需要使用setOwner来确保pipeline在销毁期间不分离
  // 这个handler，以免发生破坏排序问题。
  // See thrift/lib/cpp2/async/Cpp2Channel.cpp for an example
  template <class H>
  bool setOwner(H* handler);

  virtual void finalize() = 0;

 protected:
  template <class Context>
  void addContextFront(Context* ctx);

  void detachHandlers();

  std::vector<std::shared_ptr<PipelineContext>> ctxs_;  // 所有的PipelineContext
  std::vector<PipelineContext*> inCtxs_;  // inbound 类型的PipelineContext
  std::vector<PipelineContext*> outCtxs_; // outbound 类型的PipelineContext

 private:
  PipelineManager* manager_{nullptr};
  std::shared_ptr<folly::AsyncTransport> transport_;
  std::shared_ptr<TransportInfo> transportInfo_;

  template <class Context>
  PipelineBase& addHelper(std::shared_ptr<Context>&& ctx, bool front);

  template <class H>
  PipelineBase& removeHelper(H* handler, bool checkEqual);

  typedef std::vector<std::shared_ptr<PipelineContext>>::iterator ContextIterator;

  ContextIterator removeAt(const ContextIterator& it);

  folly::WriteFlags writeFlags_{folly::WriteFlags::NONE};
  std::pair<uint64_t, uint64_t> readBufferSettings_{2048, 2048};

  std::shared_ptr<PipelineContext> owner_;// 被哪个Context拥有
};

/*
 * R is the inbound type, i.e. inbound calls start with pipeline.read(R)
 * W is the outbound type, i.e. outbound calls start with pipeline.write(W)
 *
 * Use Unit for one of the types if your pipeline is unidirectional（单向）.
 * If R is Unit, read(), readEOF(), and readException() will be disabled.
 * If W is Unit, write() and close() will be disabled.
 */
template <class R, class W = folly::Unit>
class Pipeline : public PipelineBase {
 public:
  using Ptr = std::shared_ptr<Pipeline>;

  static Ptr create() {
    return std::shared_ptr<Pipeline>(new Pipeline());
  }

  ~Pipeline();

  // 模板方法
  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  read(R msg);//front_->read(std::forward<R>(msg)); --> this->handler_->read(this, std::forward<Rin>(msg));

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  readEOF();//front_->readEOF();

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  readException(folly::exception_wrapper e);//front_->readException(std::move(e));

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  transportActive();// front_->transportActive();

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  transportInactive();//front_->transportInactive();

  template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  write(W msg);//back_->write(std::forward<W>(msg));

  template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  writeException(folly::exception_wrapper e);//back_->writeException(std::move(e));

  template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  close();//back_->close()

  void finalize() override;

 protected:
  Pipeline();
  explicit Pipeline(bool isStatic);

 private:
  bool isStatic_{false};

  InboundLink<R>* front_{nullptr};// inbound类型Context（read）
  OutboundLink<W>* back_{nullptr};// outbound类型Context (write)
};

} // namespace wangle

namespace folly {

class AsyncSocket;
class AsyncTransportWrapper;
class AsyncUDPSocket;

}

namespace wangle {

// 默认的Pipeline定义 R = folly::IOBufQueue,W = std::unique_ptr<folly::IOBuf>
using DefaultPipeline = Pipeline<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>;

// 客户端pipeline工厂
template <typename Pipeline>
class PipelineFactory {
 public:
  // Pipeline工厂需要实现这个工厂方法
  virtual typename Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransportWrapper>) = 0;

  virtual ~PipelineFactory() = default;
};

// 连接信息
struct ConnInfo {
  folly::AsyncTransportWrapper* sock; // 异步socket指针
  const folly::SocketAddress* clientAddr;// 客户端地址
  const std::string& nextProtoName;
  SecureTransportType secureType;
  const TransportInfo& tinfo; //TransportInfo 
};

// 连接事件
enum class ConnEvent {
  CONN_ADDED,//连接添加
  CONN_REMOVED,//连接移除
};

//AcceptPipelineType可以持有的类型如下，默认类型为folly::IOBuf
typedef boost::variant<folly::IOBuf*,
                       folly::AsyncTransportWrapper*,
                       ConnInfo&,
                       ConnEvent,
                       std::tuple<folly::IOBuf*,
                                  std::shared_ptr<folly::AsyncUDPSocket>,
                                  folly::SocketAddress>> AcceptPipelineType;
typedef Pipeline<AcceptPipelineType> AcceptPipeline;// 专门用于accept的pipeline

// 服务端pipeline 工厂
class AcceptPipelineFactory {
 public:
  virtual typename AcceptPipeline::Ptr newPipeline(Acceptor* acceptor) = 0;

  virtual ~AcceptPipelineFactory() = default;
};

}

#include <wangle/channel/Pipeline-inl.h>
