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

#include <wangle/bootstrap/ServerBootstrap-inl.h>
#include <folly/Baton.h>
#include <wangle/channel/Pipeline.h>
#include <iostream>
#include <thread>

namespace wangle {

/*
  ServerBootstrap is a parent class intended to set up a
  high-performance TCP accepting server.  It will manage a pool of
  accepting threads(accept线程池), any number of accepting sockets（Listineing的socket）, a pool of
  IO-worker threads（IO线程池）, and connection pool（连接池） for each IO thread for you.

  The output is given as a Pipeline template: given a
  PipelineFactory, it will create a new pipeline for each connection,
  and your server can handle the incoming bytes.

  BACKWARDS COMPATIBLITY（向后兼容）: for servers already taking a pool of
  Acceptor objects, an AcceptorFactory can be given directly instead
  of a pipeline factory.
 */

    // 此处的Pipeline是为每个新的连接要创建的Pipeline类型
    // 创建此Pipeline的工厂在childPipeline方法中设置
    template<typename Pipeline = wangle::DefaultPipeline>
    class ServerBootstrap {
    public:
        ServerBootstrap(const ServerBootstrap &that) = delete;

        ServerBootstrap(ServerBootstrap &&that) = default;

        ServerBootstrap() = default;

        ~ServerBootstrap() {
            stop();
            join();
        }

        /*
         * Pipeline used to add connections to event bases.
         * This is used for UDP or for load balancing
         * TCP connections to IO threads explicitly(明确的)
         *
         * 设置用于创建accept pipeline的pipeline的工厂
         * 默认为DefaultAcceptPipelineFactory
         */
        ServerBootstrap *pipeline(std::shared_ptr<AcceptPipelineFactory> factory) {
            acceptPipelineFactory_ = factory;
            return this;
        }


        // 创建服务端socket的工厂，默认为AsyncServerSocketFactory
        ServerBootstrap *channelFactory(std::shared_ptr<ServerSocketFactory> factory) {
            socketFactory_ = factory;
            return this;
        }

        // 配置accept参数
        ServerBootstrap *acceptorConfig(const ServerSocketConfig &accConfig) {
            accConfig_ = accConfig;
            return this;
        }

        /*
          BACKWARDS COMPATIBILITY（向后兼容性） - an acceptor factory can be set.  Your
          Acceptor is responsible for managing the connection pool（连接池）.

          @param childHandler - acceptor factory to call for each IO thread

          类似与Netty中的ChannelInitialLizer-->ServerBootstrapAcceptor，负责一个新连接被accept之后的初始化工作
          这里就是ServerAcceptor，负责创建新连接、创建childpipeline等

          默认为ServerAcceptorFactory

          此处的childHandler与Netty4/5中的用法不一致
         */
        ServerBootstrap *childHandler(std::shared_ptr<AcceptorFactory> h) {
            acceptorFactory_ = h;
            return this;
        }

        /*
         *
         * 用于为每个新连接创建pipeline的pipeline工厂
         *
         * @param factory pipeline factory to use for each new connection
         */
        ServerBootstrap *childPipeline(std::shared_ptr<PipelineFactory<Pipeline>> factory) {
            childPipelineFactory_ = factory;
            return this;
        }

        /*
         * Set the IO executor.  If not set, a default one will be created
         * with one thread per core.(如果没有手动设置IO线程，那么自动为每个核对应一个线程)
         *
         * @param io_group - io executor to use for IO threads.
         */
        ServerBootstrap *group(std::shared_ptr<wangle::IOThreadPoolExecutor> io_group) {
            return group(nullptr, io_group);
        }

        /*
         * Set the acceptor executor, and IO executor.
         *
         * If no acceptor executor is set, a single thread will be created for accepts
         * If no IO executor is set, a default of one thread per core will be created
         *
         * @param group - acceptor executor to use for acceptor threads.
         * @param io_group - io executor to use for IO threads.
         */
        ServerBootstrap *group(
                std::shared_ptr<wangle::IOThreadPoolExecutor> accept_group,  // acceptor线程
                std::shared_ptr<wangle::IOThreadPoolExecutor> io_group) {    // io线程
            // 如果没有设置accept线程
            if (!accept_group) {
                // 就创建一个只有一个线程的线程池负责accept
                accept_group = std::make_shared<wangle::IOThreadPoolExecutor>(
                        1, std::make_shared<wangle::NamedThreadFactory>("Acceptor Thread"));
            }
            // 如果没有设置IO线程池
            if (!io_group) {
                auto threads = std::thread::hardware_concurrency();// 返回CPU核数
                if (threads <= 0) {
                    // Reasonable mid-point for concurrency when actual value unknown
                    threads = 8;
                }
                // 创建IO线程，线程数为CPU核数（这一步会真正的创建threads个线程）
                io_group = std::make_shared<wangle::IOThreadPoolExecutor>(
                        threads, std::make_shared<wangle::NamedThreadFactory>("IO Thread"));
            }

            // TODO better config checking
            // CHECK(acceptorFactory_ || childPipelineFactory_);
            // 二者必须设置其中一个
            CHECK(!(acceptorFactory_ && childPipelineFactory_));

            // 如果自己提供了定制的ServerWorkerPool
            if (acceptorFactory_) {
                workerFactory_ = std::make_shared<ServerWorkerPool>(
                        acceptorFactory_,
                        io_group.get(),
                        sockets_,
                        socketFactory_);
            } else {
                // 否则就是用默认的
                workerFactory_ = std::make_shared<ServerWorkerPool>(
                        /* ServerAcceptorFactory用于创建一个ServerAcceptor，这个ServerAcceptor<Pipeline>
                         * 负责新建acceptPipeline_，并且它自己还是一个wangle::InboundHandler
                         * 将自己添加到ServerAcceptor，负责对新的连接的创建和管理
                         * 注意在不设置时，acceptPipelineFactory_的值默认无DefaultAcceptPipelineFactory（只是单纯的创建了一个空白Pipeline）
                         * */
                        std::make_shared<ServerAcceptorFactory<Pipeline>>(acceptPipelineFactory_, childPipelineFactory_,
                                                                          accConfig_),
                        io_group.get(),
                        sockets_,// listening中的sockets，在bind调用之前这里的sockets_为空
                        socketFactory_);
            }

            // 为IO线程池添加观察者！这一步会出发调用每一个线程的threadPreviouslyStarted方法
            // workerFactory_是一个ThreadPoolExecutor::Observer
            io_group->addObserver(workerFactory_);

            acceptor_group_ = accept_group;
            io_group_ = io_group;

            return this;
        }

        /*
         * Bind to an existing socket
         *
         * @param sock Existing socket to use for accepting
         *
         * 在一個已經存在的socket上進行監聽
         */
        void bind(folly::AsyncServerSocket::UniquePtr s) {
            // 如果workerFactory_为空
            if (!workerFactory_) {
                group(nullptr);// 设置io线程池为nullptr，最终accept和io线程池都是用默认设置。并且还会撞见设置workerFactory_
            }

            // Since only a single socket is given,
            // we can only accept on a single thread
            // 因为这里只有一个socket，因此只能在一个单独的线程上accept
            CHECK(acceptor_group_->numThreads() == 1);
            // 创建异步ServerSocket
            std::shared_ptr<folly::AsyncServerSocket> socket(s.release(),
                                                             AsyncServerSocketFactory::ThreadSafeDestructor());

            // 在acceptor_group_线程池里面执行ServerSocket的accept监听
            folly::via(acceptor_group_.get(), [&] {
                socket->attachEventBase(folly::EventBaseManager::get()->getEventBase());//绑定eventbase(Netty中的eventloop)
                socket->listen(socketConfig.acceptBacklog);//启动监听，设置acceptBacklog
                socket->startAccepting();//开始accept
            }).get();//阻塞

            // Startup all the threads
            workerFactory_->forEachWorker([this, socket](Acceptor *worker) {
                // 在accept eventbase中执行
                socket->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
                        [this, worker, socket]() {
                            //worker：callback    worker->getEventBase()
                            //socket->addAcceptCallback(callback, base);
                            socketFactory_->addAcceptCB(socket, worker, worker->getEventBase());
                        });
            });

            // 添加
            sockets_->push_back(socket);
        }

        void bind(folly::SocketAddress &address) {
            bindImpl(address);
        }

        /*
         * Bind to a port and start listening.
         * One of childPipeline or childHandler must be called before bind
         *
         * @param port Port to listen on
         */
        void bind(int port) {
            CHECK(port >= 0);
            folly::SocketAddress address;
            // 设置本地地址
            address.setFromLocalPort(port);
            bindImpl(address);
        }

        // 绑定并监听这个地址
        void bindImpl(folly::SocketAddress &address) {
            // 之前没有手动设置group
            if (!workerFactory_) {
                group(nullptr);
            }

            // 如果accept线程数大于1，那么就在所有的accept线程中重用端口进行监听
            bool reusePort = reusePort_ || (acceptor_group_->numThreads() > 1);

            std::mutex sock_lock;
            std::vector<std::shared_ptr<folly::AsyncSocketBase>> new_sockets;

            std::exception_ptr exn;

            // 定义一个lamda表达式，执行ServerSocket创建和accept操作，该函数一定会在accept线程中执行
            auto startupFunc = [&](std::shared_ptr<folly::Baton<>> barrier) {

                try {
                    // 创建服务端监听socket
                    // 此函数不会阻塞
                    // AsyncServerSocketFactory
                    auto socket = socketFactory_->newSocket(address, socketConfig.acceptBacklog, reusePort,
                                                            socketConfig);
                    sock_lock.lock();
                    // 添加到new_sockets中
                    new_sockets.push_back(socket);
                    sock_lock.unlock();
                    // 获取socket绑定的本地地址
                    socket->getAddress(&address);
                    // 唤醒
                    barrier->post();
                } catch (...) {
                    // 先把异常记录下来
                    exn = std::current_exception();
                    barrier->post();

                    return;
                }

            };

            auto wait0 = std::make_shared<folly::Baton<>>();
            // 在acceptor_group_线程池中添加并执行startupFunc任务（异步）
            acceptor_group_->add(std::bind(startupFunc, wait0));
            wait0->wait();//等待

            // 从1开始，在剩下的acceptor线程中启动监听
            for (size_t i = 1; i < acceptor_group_->numThreads(); i++) {
                auto barrier = std::make_shared<folly::Baton<>>();
                acceptor_group_->add(std::bind(startupFunc, barrier));
                barrier->wait();
            }

            // 如果前面有异常
            if (exn) {
                // 异常重新抛出
                std::rethrow_exception(exn);
            }

            // 遍历new_sockets(所有新创建的listening中的socket)
            for (auto &socket : new_sockets) {
                // 遍历IO线程池
                workerFactory_->forEachWorker([this, socket](Acceptor *worker) {
                    // 在acceptor线程中执行
                    socket->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait(
                            [this, worker, socket]() {
                                // 异步的添加accept回调worker
                                socketFactory_->addAcceptCB(socket, worker, worker->getEventBase());
                            });
                });
                // 缓存所有处于listening状态的socket
                sockets_->push_back(socket);
            }
        }

        /*
         * Stop listening on all sockets.
         *
         * 在所有的sockets上停止监听
         */
        void stop() {
            // sockets_ may be null if ServerBootstrap has been std::move'd
            if (sockets_) {
                sockets_->clear();
            }
            // 如果之前没有停止
            if (!stopped_) {
                stopped_ = true;// 标记停止
                // stopBaton_ may be null if ServerBootstrap has been std::move'd
                if (stopBaton_) {
                    stopBaton_->post();// 唤醒
                }
            }
        }

        void join() {
            if (acceptor_group_) {
                acceptor_group_->join();
            }
            if (io_group_) {
                io_group_->join();
            }
        }

        // 等待结束
        void waitForStop() {
            if (!stopped_) {
                CHECK(stopBaton_);
                stopBaton_->wait();// 在Baton上睡眠等待
            }
        }

        /*
         * Get the list of listening sockets
         */
        const std::vector<std::shared_ptr<folly::AsyncSocketBase>> &
        getSockets() const {
            return *sockets_;
        }

        std::shared_ptr<wangle::IOThreadPoolExecutor> getIOGroup() const {
            return io_group_;
        }

        template<typename F>
        void forEachWorker(F &&f) const {
            if (!workerFactory_) {
                return;
            }
            workerFactory_->forEachWorker(f);
        }


        ServerSocketConfig socketConfig;

        // 设置是否重用端口
        ServerBootstrap *setReusePort(bool reusePort) {
            reusePort_ = reusePort;
            return this;
        }

    private:
        std::shared_ptr<wangle::IOThreadPoolExecutor> acceptor_group_;// 用于accept的线程池
        std::shared_ptr<wangle::IOThreadPoolExecutor> io_group_;      // 用于IO的线程池

        std::shared_ptr<ServerWorkerPool> workerFactory_;             // 线程池监视器
        std::shared_ptr<std::vector<std::shared_ptr<folly::AsyncSocketBase>>> sockets_{//处于listening中的socket
                std::make_shared<std::vector<std::shared_ptr<folly::AsyncSocketBase>>>()
        };

        std::shared_ptr<AcceptorFactory> acceptorFactory_;//创建Acceptor的工厂
        std::shared_ptr<PipelineFactory<Pipeline>> childPipelineFactory_;//为每个新连接创建pipeline的工厂
        std::shared_ptr<AcceptPipelineFactory> acceptPipelineFactory_{//accept的pipeline的工厂
                std::make_shared<DefaultAcceptPipelineFactory>()
        };
        std::shared_ptr<ServerSocketFactory> socketFactory_{// 创建AsyncServerSocket的工厂，默认为AsyncServerSocketFactory
                std::make_shared<AsyncServerSocketFactory>()
        };

        ServerSocketConfig accConfig_;// accept配置

        bool reusePort_{false};//是否重用端口

        std::unique_ptr<folly::Baton<>> stopBaton_{folly::make_unique<folly::Baton<>>()};
        bool stopped_{false};
    };

} // namespace wangle
