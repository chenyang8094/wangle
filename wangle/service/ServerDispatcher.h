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

/**
  每次从管道中同步派发请求。并发请求会在管道中排队。
 */
template <typename Req, typename Resp = Req>
class SerialServerDispatcher : public HandlerAdapter<Req, Resp> {
public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit SerialServerDispatcher(Service<Req, Resp>* service) : service_(service) {}

  void read(Context* ctx, Req in) override {
    auto resp = (*service_)(std::move(in)).get();// 开始调用，同步
    ctx->fireWrite(std::move(resp));// 写回响应
  }

private:

  Service<Req, Resp>* service_;
};

/**
  调度来自管道的请求。响应在队列中等待，直到它们可以按顺序发送。
 */
template <typename Req, typename Resp = Req>
class PipelinedServerDispatcher : public HandlerAdapter<Req, Resp> {
public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit PipelinedServerDispatcher(Service<Req, Resp>* service)
    : service_(service) {}

  void read(Context*, Req in) override {
    auto requestId = requestId_++;
    (*service_)(std::move(in)).then([requestId, this](Resp & resp) {
      responses_[requestId] = resp;
      sendResponses();
    });
  }

  void sendResponses() {
    auto search = responses_.find(lastWrittenId_ + 1);
    while (search != responses_.end()) {
      Resp resp = std::move(search->second);
      responses_.erase(search->first);
      this->getContext()->fireWrite(std::move(resp));
      lastWrittenId_++;
      search = responses_.find(lastWrittenId_ + 1);
    }
  }

private:
  Service<Req, Resp>* service_;
  uint32_t requestId_{1};
  std::unordered_map<uint32_t, Resp> responses_;
  uint32_t lastWrittenId_{0};
};

/**
  Dispatch requests from pipeline as they come in.  Concurrent
  requests are assumed to have sequence id's that are taken care of
  by the pipeline.  Unlike a multiplexed client dispatcher, a
  multiplexed server dispatcher needs no state, and the sequence id's
  can just be copied from the request to the response in the pipeline.

  从管道中分派请求。并发请求被假定具有被pipeline处理的序列ID。与多路复用的客户分派器不同,
  多路复用的服务器分派器不需要状态，并且序列ID可以只是从请求复制到流水线中的响应。
 */
template <typename Req, typename Resp = Req>
class MultiplexServerDispatcher : public HandlerAdapter<Req, Resp> {
public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit MultiplexServerDispatcher(Service<Req, Resp>* service) : service_(service) {}
   
  // 接收request
  void read(Context* ctx, Req in) override {
    // 开始调用
    (*service_)(std::move(in)).then([ctx](Resp resp) {
      ctx->fireWrite(std::move(resp));
    });
  }

private:
  Service<Req, Resp>* service_;
};

} // namespace wangle
