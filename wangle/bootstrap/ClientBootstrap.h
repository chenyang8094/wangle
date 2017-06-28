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

#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBaseManager.h>
#include <wangle/bootstrap/BaseClientBootstrap.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/concurrent/IOThreadPoolExecutor.h>

namespace wangle {

/*
 * A thin wrapper around Pipeline and AsyncSocket to match
 * ServerBootstrap.  On connect() a new pipeline is created.
 *
 * 其实wangle中的
 */
template <typename Pipeline>
class ClientBootstrap : public BaseClientBootstrap<Pipeline> {
  class ConnectCallback : public folly::AsyncSocket::ConnectCallback {
   public:
    // 异步连接的回调
    ConnectCallback(folly::Promise<Pipeline*> promise, ClientBootstrap* bootstrap)
        : promise_(std::move(promise))
        , bootstrap_(bootstrap) {}

    void connectSuccess() noexcept override {
      if (bootstrap_->getPipeline()) {
        bootstrap_->getPipeline()->transportActive();// 在pipeline中传播Active事件
      }
      promise_.setValue(bootstrap_->getPipeline());  // 设置promise，值为pipeline
      delete this;
    }

    void connectErr(const folly::AsyncSocketException& ex) noexcept override {
      promise_.setException(                         // 设置promise，值为对应的异常
        folly::make_exception_wrapper<folly::AsyncSocketException>(ex));
      delete this;
    }
   private:
    folly::Promise<Pipeline*> promise_;  // 持有的promise
    ClientBootstrap* bootstrap_;         // 绑定的ClientBootstrap
  };

 public:
  ClientBootstrap() {
  }

  // 指定IO线程池
  ClientBootstrap* group(
      std::shared_ptr<wangle::IOThreadPoolExecutor> group) {
    group_ = group;
    return this;
  }

  ClientBootstrap* bind(int port) {
    port_ = port;
    return this;
  }

  // 发起异步连接操作（带超时）
  folly::Future<Pipeline*> connect(
      const folly::SocketAddress& address,
      std::chrono::milliseconds timeout =
          std::chrono::milliseconds(0)) override {
    // 如果指定了io线程池，就从IO线程池中取eventbase，否则就从folly::EventBaseManager取
    auto base = (group_)  
      ? group_->getEventBase()
      : folly::EventBaseManager::get()->getEventBase();
    //  定义一个future，用于异步获取connect结果（值为一个pipeline*）
    folly::Future<Pipeline*> retval((Pipeline*)nullptr);
    base->runImmediatelyOrRunInEventBaseThreadAndWait([&](){
      // 定义一个异步socket
      std::shared_ptr<folly::AsyncSocket> socket;
      if (this->sslContext_) {
        auto sslSocket =
            folly::AsyncSSLSocket::newSocket(this->sslContext_, base);
        if (this->sslSession_) {
          sslSocket->setSSLSession(this->sslSession_, true);
        }
        socket = sslSocket;
      } else {
        // 创建一个异步socket
        socket = folly::AsyncSocket::newSocket(base);
      }
      // 定义promise并获取future
      folly::Promise<Pipeline*> promise;
      retval = promise.getFuture();
      // 发起异步连接，并将promise传给异步回调
      socket->connect(
          new ConnectCallback(std::move(promise), this),
          address,
          timeout.count());
      //  pipeline_ = pipelineFactory_->newPipeline(socket);
      this->makePipeline(socket);
    });
    return retval;// 返回这个future
  }

  virtual ~ClientBootstrap() = default;

 protected:
  int port_; // 绑定的端口
  std::shared_ptr<wangle::IOThreadPoolExecutor> group_;// IO线程池
};

// ClientBootstrap工厂，继承BaseClientBootstrapFactory
class ClientBootstrapFactory
    : public BaseClientBootstrapFactory<BaseClientBootstrap<>> {
 public:
  ClientBootstrapFactory() {}
  // 复写抽象工厂方法
  BaseClientBootstrap<>::Ptr newClient() override {
    return folly::make_unique<ClientBootstrap<DefaultPipeline>>();
  }
};

} // namespace wangle
