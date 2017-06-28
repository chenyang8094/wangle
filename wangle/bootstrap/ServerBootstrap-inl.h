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

#include <folly/ExceptionWrapper.h>
#include <folly/SharedMutex.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventBaseManager.h>
#include <wangle/acceptor/Acceptor.h>
#include <wangle/acceptor/ManagedConnection.h>
#include <wangle/bootstrap/ServerSocketFactory.h>
#include <wangle/channel/Handler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/concurrent/IOThreadPoolExecutor.h>
#include <wangle/ssl/SSLStats.h>

namespace wangle {

    class AcceptorException : public std::runtime_error {
    public:
        enum class ExceptionType {
            UNKNOWN = 0,
            TIMED_OUT = 1,
            DROPPED = 2,
            ACCEPT_STOPPED = 3,
            DRAIN_CONN_PCT = 4,
            DROP_CONN_PCT = 5,
            FORCE_STOP = 6,
            INTERNAL_ERROR = 7,
        };

        explicit AcceptorException(ExceptionType type) :
                std::runtime_error(""), type_(type), pct_(0.0) {}

        explicit AcceptorException(ExceptionType type, const std::string &message) :
                std::runtime_error(message), type_(type), pct_(0.0) {}

        explicit AcceptorException(ExceptionType type, const std::string &message,
                                   double pct) :
                std::runtime_error(message), type_(type), pct_(pct) {}

        ExceptionType getType() const noexcept { return type_; }

        double getPct() const noexcept { return pct_; }

    protected:
        const ExceptionType type_;
        // the percentage of connections to be drained or dropped during the shutdown
        const double pct_;
    };

// 牛逼，还是一个InboundHandler，这个Handler的R的类型为AcceptPipelineType
    template<typename Pipeline>
    class ServerAcceptor : public Acceptor, public wangle::InboundHandler<AcceptPipelineType> {
    public:
        // 内部类（代表一个连接），这是一个可以被管理（Managed）的连接
        class ServerConnection : public wangle::ManagedConnection, public wangle::PipelineManager {
        public:
            explicit ServerConnection(typename Pipeline::Ptr pipeline)
                    : pipeline_(std::move(pipeline)) {
                pipeline_->setPipelineManager(this);
            }

            void timeoutExpired() noexcept override {
                auto ew = folly::make_exception_wrapper<AcceptorException>(
                        AcceptorException::ExceptionType::TIMED_OUT, "timeout");
                pipeline_->readException(ew);
            }

            void describe(std::ostream &) const override {}

            bool isBusy() const override {
                return true;
            }

            void notifyPendingShutdown() override {}

            void closeWhenIdle() override {}

            // 销毁连接
            void dropConnection() override {
                DestructorGuard dg(this);
                auto ew = folly::make_exception_wrapper<AcceptorException>(
                        AcceptorException::ExceptionType::DROPPED, "dropped");
                pipeline_->readException(ew);
                destroy();
            }

            void dumpConnectionState(uint8_t /* loglevel */) override {}

            // PipelineManager
            void deletePipeline(wangle::PipelineBase *p) override {
                CHECK(p == pipeline_.get());
                destroy();
            }

            // 初始化这个连接
            void init() {
                // 在该连接绑定的pipeline中引发Active事件
                pipeline_->transportActive();
            }

            // PipelineManager
            void refreshTimeout() override {
                resetTimeout();//If the connection has a connection manager, reset the timeout countdown to connection manager's default timeout.
            }

        private:
            ~ServerConnection() {
                pipeline_->setPipelineManager(nullptr);
            }

            typename Pipeline::Ptr pipeline_;
        };

        // 显示的构造函数
        explicit ServerAcceptor(
                std::shared_ptr<AcceptPipelineFactory> acceptPipelineFactory,
                std::shared_ptr<PipelineFactory<Pipeline>> childPipelineFactory,
                const ServerSocketConfig &accConfig)
                : Acceptor(accConfig),
                  acceptPipelineFactory_(acceptPipelineFactory),
                  childPipelineFactory_(childPipelineFactory) {
        }

        //  Accept被创建之后会被调用该方法进行初始化
        void init(folly::AsyncServerSocket *serverSocket,
                  folly::EventBase *eventBase,
                  SSLStats *stats = nullptr) override {

            // eventBase为io线程的
            Acceptor::init(serverSocket, eventBase, stats);

            // 创建acceptPipeline，参数为Acceptor
            acceptPipeline_ = acceptPipelineFactory_->newPipeline(this);

            // 如果设置了childPipelineFactory，这就意味着没有自己提供定制的AcceptPipelineFactory
            // 而是采用了默认的，因此需要将ServerAcceptor（本身也是一个Inbound Handler）也添加到
            // AcceptPipeline
            if (childPipelineFactory_) {
                // This means a custom AcceptPipelineFactory was not passed in via
                // pipeline() and we're using the DefaultAcceptPipelineFactory.
                // Add the default inbound handler here.
                acceptPipeline_->addBack(this);
            }

            acceptPipeline_->finalize();
        }

        // 当一个新的客户端连接onNewConnection过来时，会触发read事件
        void read(Context *, AcceptPipelineType conn) override {
            if (conn.type() != typeid(ConnInfo &)) {
                return;
            }

            auto connInfo = boost::get<ConnInfo &>(conn);

            folly::AsyncTransportWrapper::UniquePtr transport(connInfo.sock);

            // Setup local and remote addresses
            auto tInfoPtr = std::make_shared<TransportInfo>(connInfo.tinfo);

            tInfoPtr->localAddr = std::make_shared<folly::SocketAddress>(accConfig_.bindAddress);

            transport->getLocalAddress(tInfoPtr->localAddr.get());

            tInfoPtr->remoteAddr = std::make_shared<folly::SocketAddress>(*connInfo.clientAddr);

            tInfoPtr->appProtocol = std::make_shared<std::string>(connInfo.nextProtoName);

            // 为新连接创建一个pipeline(参数为AsyncTransport)
            auto pipeline = childPipelineFactory_->newPipeline(
                    std::shared_ptr<folly::AsyncTransportWrapper>(
                            transport.release(), folly::DelayedDestruction::Destructor()));

            // 设置TransportInfo
            pipeline->setTransportInfo(tInfoPtr);

            // 创建一个新的可被管理的连接,并绑定pipeline（相当于Netty中的Channel）
            auto connection = new ServerConnection(std::move(pipeline));
            // 将连接管理起来
            Acceptor::addConnection(connection);
            // 初始化这个连接
            connection->init();
        }

        // Null implementation to terminate the call in this handler
        // and suppress warnings
        void readEOF(Context *) override {}

        void readException(Context *, folly::exception_wrapper) override {}

        // 当有新连接时   handlerReady->consumeMessages->messageAvailable->connectionAccepted（folly::AsyncServerSocket::AcceptCallback）->onDoneAcceptingConnection
        // ->processEstablishedConnection->plaintextConnectionReady->connectionReady->onNewConnection
        void onNewConnection(folly::AsyncTransportWrapper::UniquePtr transport,
                             const folly::SocketAddress *clientAddr,
                             const std::string &nextProtocolName,
                             SecureTransportType secureTransportType,
                             const TransportInfo &tinfo) override {

            ConnInfo connInfo = {transport.release(), clientAddr, nextProtocolName, secureTransportType, tinfo};
            // 在acceptPipeline传播read
            acceptPipeline_->read(connInfo);
        }

        // notify the acceptors in the acceptPipeline to drain & drop conns
        void acceptStopped() noexcept override {
            auto ew = folly::make_exception_wrapper<AcceptorException>(
                    AcceptorException::ExceptionType::ACCEPT_STOPPED,
                    "graceful shutdown timeout");

            acceptPipeline_->readException(ew);
            Acceptor::acceptStopped();
        }

        void drainConnections(double pct) noexcept override {
            auto ew = folly::make_exception_wrapper<AcceptorException>(
                    AcceptorException::ExceptionType::DRAIN_CONN_PCT,
                    "draining some connections", pct);

            acceptPipeline_->readException(ew);
            Acceptor::drainConnections(pct);
        }

        void dropConnections(double pct) noexcept override {
            auto ew = folly::make_exception_wrapper<AcceptorException>(
                    AcceptorException::ExceptionType::DROP_CONN_PCT,
                    "dropping some connections", pct);

            acceptPipeline_->readException(ew);
            Acceptor::dropConnections(pct);
        }

        void forceStop() noexcept override {
            auto ew = folly::make_exception_wrapper<AcceptorException>(
                    AcceptorException::ExceptionType::FORCE_STOP,
                    "hard shutdown timeout");

            acceptPipeline_->readException(ew);
            Acceptor::forceStop();
        }

        // UDP thunk
        void onDataAvailable(std::shared_ptr<folly::AsyncUDPSocket> socket,
                             const folly::SocketAddress &addr,
                             std::unique_ptr<folly::IOBuf> buf,
                             bool /* truncated */) noexcept override {
            acceptPipeline_->read(
                    AcceptPipelineType(make_tuple(buf.release(), socket, addr)));
        }

        void onConnectionAdded(const wangle::ConnectionManager &) override {
            acceptPipeline_->read(ConnEvent::CONN_ADDED);
        }

        void onConnectionRemoved(const wangle::ConnectionManager &) override {
            acceptPipeline_->read(ConnEvent::CONN_REMOVED);
        }

        void sslConnectionError(const folly::exception_wrapper &ex) override {
            acceptPipeline_->readException(ex);
            Acceptor::sslConnectionError(ex);
        }

    private:
        std::shared_ptr<AcceptPipelineFactory> acceptPipelineFactory_;//accept的pipeline工厂
        std::shared_ptr<AcceptPipeline> acceptPipeline_;//accept的pipeline
        std::shared_ptr<PipelineFactory<Pipeline>> childPipelineFactory_;//accept的pipeline
    };

    // ServerAcceptor工厂
    template<typename Pipeline>
    class ServerAcceptorFactory : public AcceptorFactory {
    public:
        explicit ServerAcceptorFactory(
                std::shared_ptr<AcceptPipelineFactory> acceptPipelineFactory,
                std::shared_ptr<PipelineFactory<Pipeline>> childPipelineFactory,
                const ServerSocketConfig &accConfig)
                : acceptPipelineFactory_(acceptPipelineFactory),
                  childPipelineFactory_(childPipelineFactory),
                  accConfig_(accConfig) {}

        // 创建ServerAcceptor
        std::shared_ptr<Acceptor> newAcceptor(folly::EventBase *base) {
            auto acceptor = std::make_shared<ServerAcceptor<Pipeline>>(acceptPipelineFactory_, childPipelineFactory_,
                                                                       accConfig_);
            // 初始化这个acceptor
            acceptor->init(nullptr, base, nullptr);
            return acceptor;
        }

    private:
        std::shared_ptr<AcceptPipelineFactory> acceptPipelineFactory_;//accept的pipeline工厂
        std::shared_ptr<PipelineFactory<Pipeline>> childPipelineFactory_;//每个连接创建pipeline的工厂
        ServerSocketConfig accConfig_;//accept标识
    };

// 线程池监视器，在线程启动、停止等都会触发
    class ServerWorkerPool : public wangle::ThreadPoolExecutor::Observer {
    public:
        explicit ServerWorkerPool(
                std::shared_ptr<AcceptorFactory> acceptorFactory,
                wangle::IOThreadPoolExecutor *exec, // IO线程池
                std::shared_ptr<std::vector<std::shared_ptr<folly::AsyncSocketBase>>> sockets,
                std::shared_ptr<ServerSocketFactory> socketFactory)
                : workers_(std::make_shared<WorkerMap>()), workersMutex_(std::make_shared<Mutex>()),
                  acceptorFactory_(acceptorFactory), exec_(exec), sockets_(sockets), socketFactory_(socketFactory) {
            CHECK(exec);
        }

        template<typename F>
        void forEachWorker(F &&f) const;

        // 线程启动时
        void threadStarted(wangle::ThreadPoolExecutor::ThreadHandle *);

        // 线程停止时
        void threadStopped(wangle::ThreadPoolExecutor::ThreadHandle *);

        void threadPreviouslyStarted(wangle::ThreadPoolExecutor::ThreadHandle *thread) {
            threadStarted(thread);
        }

        void threadNotYetStopped(wangle::ThreadPoolExecutor::ThreadHandle *thread) {
            threadStopped(thread);
        }

    private:
        // 线程句柄与Acceptor之间的映射关系
        using WorkerMap = std::map<wangle::ThreadPoolExecutor::ThreadHandle *, std::shared_ptr<Acceptor>>;

        using Mutex = folly::SharedMutexReadPriority;

        std::shared_ptr<WorkerMap> workers_;

        std::shared_ptr<Mutex> workersMutex_;

        // 创建ServerAcceptor工厂，ServerAcceptor负责创建新连接、创建childpipeline等
        std::shared_ptr<AcceptorFactory> acceptorFactory_;
        // IO线程
        wangle::IOThreadPoolExecutor *exec_{nullptr};

        // 监听中的server socket
        std::shared_ptr<std::vector<std::shared_ptr<folly::AsyncSocketBase>>> sockets_;
        // 服务端监听socket的创建工厂
        std::shared_ptr<ServerSocketFactory> socketFactory_;
    };

    template<typename F>
    void ServerWorkerPool::forEachWorker(F &&f) const {
        Mutex::ReadHolder holder(workersMutex_.get());
        for (const auto &kv : *workers_) {// 遍历所有的Acceptor
            f(kv.second.get());// 应用函数f
        }
    }

    class DefaultAcceptPipelineFactory : public AcceptPipelineFactory {
    public:
        typename AcceptPipeline::Ptr newPipeline(Acceptor *) {
            return AcceptPipeline::create();
        }
    };

} // namespace wangle
