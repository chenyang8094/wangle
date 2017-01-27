/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/concurrent/NamedThreadFactory.h>
#include <wangle/channel/Handler.h>
#include <folly/io/async/EventBaseManager.h>

namespace wangle {

// io_group->addObserver(workerFactory_);时调用
void ServerWorkerPool::threadStarted(wangle::ThreadPoolExecutor::ThreadHandle* h) {
  // 创建一个ServerAcceptor，该Acceptor绑定到一个线程池中
  auto worker = acceptorFactory_->newAcceptor(exec_->getEventBase(h));
  {
    Mutex::WriteHolder holder(workersMutex_.get());
    // 插入映射（线程句柄、ServerAcceptor）
    workers_->insert({h, worker});
  }

  // 遍历所有Listening中的socket
  for(auto socket : *sockets_) {
    socket->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this, worker, socket](){
        // 添加accept回调为ServerAcceptor
        socketFactory_->addAcceptCB(socket, worker.get(), worker->getEventBase());
    });
  }
}

// 线程停止时
void ServerWorkerPool::threadStopped(wangle::ThreadPoolExecutor::ThreadHandle* h) {
  auto worker = [&] {
    Mutex::WriteHolder holder(workersMutex_.get());
    auto workerIt = workers_->find(h);
    CHECK(workerIt != workers_->end());
    auto w = std::move(workerIt->second);
    workers_->erase(workerIt); //  移除映射
    return w;
  }();

  for (auto socket : *sockets_) {
    socket->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [&]() {
        socketFactory_->removeAcceptCB(socket, worker.get(), nullptr);
    });
  }

  worker->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
    [&]() {
      worker->dropAllConnections();
    });
}

} // namespace wangle
