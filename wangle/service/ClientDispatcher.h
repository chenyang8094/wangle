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

#include <wangle/channel/Handler.h>
#include <wangle/service/Service.h>

namespace wangle {

template <typename Pipeline, typename Req, typename Resp = Req>
class ClientDispatcherBase : public HandlerAdapter<Resp, Req>, public Service<Req, Resp> {
 public:
  typedef typename HandlerAdapter<Resp, Req>::Context Context;

  ~ClientDispatcherBase() {
    if (pipeline_) {
      try {
        // 把自己从Pipeline中删除
        pipeline_->remove(this).finalize();
      } catch (const std::invalid_argument& e) {
        // not in pipeline; this is fine
      }
    }
  }

  void setPipeline(Pipeline* pipeline) {
    try {
      pipeline->template remove<ClientDispatcherBase>();
    } catch (const std::invalid_argument& e) {
      // no existing dispatcher; this is fine
    }
    pipeline_ = pipeline;
    // 把自己添加到Pipeline
    pipeline_->addBack(this);
    pipeline_->finalize();
  }

  virtual folly::Future<folly::Unit> close() override {
    return HandlerAdapter<Resp, Req>::close(this->getContext());
  }

  virtual folly::Future<folly::Unit> close(Context* ctx) override {
    return HandlerAdapter<Resp, Req>::close(ctx);
  }

 protected:
  Pipeline* pipeline_{nullptr};
};

/**

  派发一个请求，承诺一个Promise`p`包含响应;当接收到响应时，返回的Future会被填充：
  一次只允许一个请求。
 */
template <typename Pipeline, typename Req, typename Resp = Req>
class SerialClientDispatcher : public ClientDispatcherBase<Pipeline, Req, Resp> {
 public:
  typedef typename HandlerAdapter<Resp, Req>::Context Context;

  // 接收response
  void read(Context*, Resp in) override {
    // p_必须已经初始化
    DCHECK(p_);
    // 填充Promise
    p_->setValue(std::move(in));
    // 清楚p_初始化标记，为下次请求做准备
    p_ = folly::none;
  }

  // 发送request
  virtual folly::Future<Resp> operator()(Req arg) override {
    // p_必须是第一次初始化
    CHECK(!p_);
    DCHECK(this->pipeline_);

    // 创建Promise
    p_ = folly::Promise<Resp>();
    // 获取结果Future
    auto f = p_->getFuture();
    this->pipeline_->write(std::move(arg));
    return f;
  }

 private:
  folly::Optional<folly::Promise<Resp>> p_;// 注意Optional标记p_是否初始化过
};

/**
 * Dispatch a request, satisfying Promise `p` with the response;
 * the returned Future is satisfied when the response is received.
 * A deque of promises/futures are mantained for pipelining.
 */
template <typename Pipeline, typename Req, typename Resp = Req>
class PipelinedClientDispatcher : public ClientDispatcherBase<Pipeline, Req, Resp> {
 public:

  typedef typename HandlerAdapter<Resp, Req>::Context Context;

  void read(Context*, Resp in) override {
    // 队列大小至少为1
    DCHECK(p_.size() >= 1);
    // 按顺序取出promise
    auto p = std::move(p_.front());
    p_.pop_front();
    p.setValue(std::move(in));
  }

  virtual folly::Future<Resp> operator()(Req arg) override {
    DCHECK(this->pipeline_);

    folly::Promise<Resp> p;
    auto f = p.getFuture();
    // 添加到队列
    p_.push_back(std::move(p));
    this->pipeline_->write(std::move(arg));
    return f;
  }

 private:
  std::deque<folly::Promise<Resp>> p_;//区别
};

/*
 * A full out-of-order request/response client would require some sort
 * of sequence id on the wire.  Currently this is left up to
 * individual protocol writers to implement.
 */

} // namespace wangle
