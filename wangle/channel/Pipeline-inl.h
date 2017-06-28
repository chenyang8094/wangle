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

#include <glog/logging.h>

namespace wangle {


// Pipeline默认构造
template <class R, class W>
Pipeline<R, W>::Pipeline() : isStatic_(false) {}

// Pipeline带构造
template <class R, class W>
Pipeline<R, W>::Pipeline(bool isStatic) : isStatic_(isStatic) {
  CHECK(isStatic_);
}

// Pipeline析构
template <class R, class W>
Pipeline<R, W>::~Pipeline() {
  if (!isStatic_) {
    detachHandlers();// 如果不是静态的，就删除所有的Handler
  }
}

// 在pipeline最后添加一个handler
template <class H>
PipelineBase& PipelineBase::addBack(std::shared_ptr<H> handler) {
  typedef typename ContextType<H>::type Context;// 声明Conetxt类型
  // 使用Context包装Handler后，将其添加到pipeline中，Context中还持有pipeline的引用
  return addHelper(
           std::make_shared<Context>(shared_from_this(), std::move(handler)),
           false);// false标识添加到尾部
}

template <class H>
PipelineBase& PipelineBase::addBack(H&& handler) {
  return addBack(std::make_shared<H>(std::forward<H>(handler)));
}

// 以Handler裸指针的形式添加Handler
template <class H>
PipelineBase& PipelineBase::addBack(H* handler) {
  /*
  template< class Y, class Deleter >
  shared_ptr( Y* ptr, Deleter d );
   */
  return addBack(std::shared_ptr<H>(handler, [](H*) {}));
}

template <class H>
PipelineBase& PipelineBase::addFront(std::shared_ptr<H> handler) {
  typedef typename ContextType<H>::type Context;
  return addHelper(
           std::make_shared<Context>(shared_from_this(), std::move(handler)),
           true);
}

template <class H>
PipelineBase& PipelineBase::addFront(H&& handler) {
  return addFront(std::make_shared<H>(std::forward<H>(handler)));
}

template <class H>
PipelineBase& PipelineBase::addFront(H* handler) {
  return addFront(std::shared_ptr<H>(handler, [](H*) {}));
}

template <class H>
PipelineBase& PipelineBase::removeHelper(H* handler, bool checkEqual) {
  typedef typename ContextType<H>::type Context;
  bool removed = false;
  for (auto it = ctxs_.begin(); it != ctxs_.end(); it++) {
    auto ctx = std::dynamic_pointer_cast<Context>(*it);
    if (ctx && (!checkEqual || ctx->getHandler() == handler)) {
      it = removeAt(it);
      removed = true;
      if (it == ctxs_.end()) {
        break;
      }
    }
  }

  if (!removed) {
    throw std::invalid_argument("No such handler in pipeline");
  }

  return *this;
}

template <class H>
PipelineBase& PipelineBase::remove() {
  return removeHelper<H>(nullptr, false);
}

template <class H>
PipelineBase& PipelineBase::remove(H* handler) {
  return removeHelper<H>(handler, true);
}

// 获取索引i对应的Handler
template <class H>
H* PipelineBase::getHandler(int i) {
  return getContext<H>(i)->getHandler();// 先找到Context,在得到其绑定的Handler
}

template <class H>
H* PipelineBase::getHandler() {
  auto ctx = getContext<H>();
  return ctx ? ctx->getHandler() : nullptr;
}

// 根据索引i获取Context
template <class H>
typename ContextType<H>::type* PipelineBase::getContext(int i) {
  auto ctx = dynamic_cast<typename ContextType<H>::type*>(ctxs_[i].get());
  CHECK(ctx);
  return ctx;
}

// 找到第一个Context并返回
template <class H>
typename ContextType<H>::type* PipelineBase::getContext() {
  for (auto pipelineCtx : ctxs_) {
    auto ctx = dynamic_cast<typename ContextType<H>::type*>(pipelineCtx.get());
    if (ctx) {
      return ctx;
    }
  }
  return nullptr;
}

// 设置Handler
template <class H>
bool PipelineBase::setOwner(H* handler) {
  typedef typename ContextType<H>::type Context;//typename:用来告诉ContextType<H>::type是一个已知的类型
                                                //此处是根据Handler类型来声明相应的Context类型
  
  // 遍历该pipeline中所有的Context
  for (auto& ctx : ctxs_) {
    auto ctxImpl = dynamic_cast<Context*>(ctx.get());
    // 找到handler对应的Context
    if (ctxImpl && ctxImpl->getHandler() == handler) {
      owner_ = ctx;//设置这个Context
      return true;
    }
  }
  return false;
}

template <class Context>
void PipelineBase::addContextFront(Context* ctx) {
  addHelper(std::shared_ptr<Context>(ctx, [](Context*) {}), true);
}
// Context添加助手,ctx:Context右值引用，front：标识加载begin还是end
template <class Context>
PipelineBase& PipelineBase::addHelper(std::shared_ptr<Context>&& ctx,bool front) {
  // 先加入总的Context （std::vector<std::shared_ptr<PipelineContext>>）
  // 该vector种使用的是智能指针，可以保持对Context的引用
  ctxs_.insert(front ? ctxs_.begin() : ctxs_.end(), ctx);
  // 然后根据方向（BOTH、IN、OUT分别加入相应的vector中）
  // std::vector<PipelineContext*> 这里放的是Context的指针，因为引用在上面的容器中已经保持
  if (Context::dir == HandlerDir::BOTH || Context::dir == HandlerDir::IN) {
    inCtxs_.insert(front ? inCtxs_.begin() : inCtxs_.end(), ctx.get());
  }
  if (Context::dir == HandlerDir::BOTH || Context::dir == HandlerDir::OUT) {
    outCtxs_.insert(front ? outCtxs_.begin() : outCtxs_.end(), ctx.get());
  }
  return *this;
}

namespace detail {

template <class T>
inline void logWarningIfNotUnit(const std::string& warning) {
  LOG(WARNING) << warning;
}

template <>
inline void logWarningIfNotUnit<folly::Unit>(const std::string& /*warning*/) {
  // do nothing
}

} // detail

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value >::type
Pipeline<R, W>::read(R msg) {
  if (!front_) {
    throw std::invalid_argument("read(): no inbound handler in Pipeline");
  }
  front_->read(std::forward<R>(msg));
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value >::type
Pipeline<R, W>::readEOF() {
  if (!front_) {
    throw std::invalid_argument("readEOF(): no inbound handler in Pipeline");
  }
  front_->readEOF();
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value >::type
Pipeline<R, W>::transportActive() {
  if (front_) {
    front_->transportActive();
  }
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value >::type
Pipeline<R, W>::transportInactive() {
  if (front_) {
    front_->transportInactive();
  }
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value >::type
Pipeline<R, W>::readException(folly::exception_wrapper e) {
  if (!front_) {
    throw std::invalid_argument(
      "readException(): no inbound handler in Pipeline");
  }
  front_->readException(std::move(e));
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value,
         folly::Future<folly::Unit >>::type
Pipeline<R, W>::write(W msg) {
  if (!back_) {
    throw std::invalid_argument("write(): no outbound handler in Pipeline");
  }
  return back_->write(std::forward<W>(msg));
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value,
         folly::Future<folly::Unit >>::type
Pipeline<R, W>::writeException(folly::exception_wrapper e) {
  if (!back_) {
    throw std::invalid_argument(
      "writeException(): no outbound handler in Pipeline");
  }
  return back_->writeException(std::move(e));
}

template <class R, class W>
template <class T>
typename std::enable_if < !std::is_same<T, folly::Unit>::value,
         folly::Future<folly::Unit >>::type
Pipeline<R, W>::close() {
  if (!back_) {
    throw std::invalid_argument("close(): no outbound handler in Pipeline");
  }
  return back_->close();// write
}

// TODO Have read/write/etc check that pipeline has been finalized
// 在调用pipeline的finalize之后，会把之前添加的所有Context按照不同的类型
// 添加到front_和back_链表中
template <class R, class W>
void Pipeline<R, W>::finalize() {
  front_ = nullptr;
  if (!inCtxs_.empty()) {
    front_ = dynamic_cast<InboundLink<R>*>(inCtxs_.front());
    for (size_t i = 0; i < inCtxs_.size() - 1; i++) {
      inCtxs_[i]->setNextIn(inCtxs_[i + 1]);
    }
    inCtxs_.back()->setNextIn(nullptr);
  }

  back_ = nullptr;
  if (!outCtxs_.empty()) {
    back_ = dynamic_cast<OutboundLink<W>*>(outCtxs_.back());
    for (size_t i = outCtxs_.size() - 1; i > 0; i--) {
      // PipelineContext
      // auto nextOut = dynamic_cast<OutboundLink<typename H::wout> *>(ctx);
      // nextOut_ = nextOut;
      outCtxs_[i]->setNextOut(outCtxs_[i - 1]);
    }
    outCtxs_.front()->setNextOut(nullptr);
  }

  if (!front_) {
    detail::logWarningIfNotUnit<R>(
      "No inbound handler in Pipeline, inbound operations will throw "
      "std::invalid_argument");
  }
  if (!back_) {
    detail::logWarningIfNotUnit<W>(
      "No outbound handler in Pipeline, outbound operations will throw "
      "std::invalid_argument");
  }

  for (auto it = ctxs_.rbegin(); it != ctxs_.rend(); it++) {
    // PipelineContext
    // this->attachContext(handler_.get(), impl_);// 将该Context绑定到handler上
    // handler_->attachPipeline(impl_); // 调用Handler的attachPipeline，有具体的Handler实现
    // attached_ = true;//标记Context已经attached到一个pipeline中
    (*it)->attachPipeline();
  }
}

} // namespace wangle
