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

namespace wangle {

    class PipelineContext {
    public:
        virtual ~PipelineContext() = default;

        // 依附到一个pipeline中
        virtual void attachPipeline() = 0;

        // 从pipeline中分离
        virtual void detachPipeline() = 0;

        // 将一个HandlerContext绑定到handler上
        // ContextImplBase->attachPipeline->attachContext/handler_->attachPipeline
        template<class H, class HandlerContext>
        void attachContext(H *handler, HandlerContext *ctx) {
            // 只有第一次绑定的时候才会设置
            if (++handler->attachCount_ == 1) {
                handler->ctx_ = ctx;
            } else {
                // 为何在此设置的时候就为nullptr
                handler->ctx_ = nullptr;
            }
        }

        // 设置下一个inbound类型的Context
        virtual void setNextIn(PipelineContext *ctx) = 0;

        // 设置下一个outbound类型的Context
        virtual void setNextOut(PipelineContext *ctx) = 0;

        // 获取方向(Context方向依赖于Handler方向)
        virtual HandlerDir getDirection() = 0;
    };

// Inbound类型链表（其中定义的都是Handler方法，注意：没有fire前缀）
    template<class In>
    class InboundLink {
    public:
        virtual ~InboundLink() = default;

        virtual void read(In msg) = 0;

        virtual void readEOF() = 0;

        virtual void readException(folly::exception_wrapper e) = 0;

        virtual void transportActive() = 0;

        virtual void transportInactive() = 0;
    };

// Outbound类型链表
    template<class Out>
    class OutboundLink {
    public:
        virtual ~OutboundLink() = default;

        virtual folly::Future<folly::Unit> write(Out msg) = 0;

        virtual folly::Future<folly::Unit> writeException(
                folly::exception_wrapper e) = 0;

        virtual folly::Future<folly::Unit> close() = 0;
    };


// PipelineContext实现基类，H为handler类型，Context为Conetxt类型
    template<class H, class Context>
    class ContextImplBase : public PipelineContext {
    public:
        ~ContextImplBase() = default;

        // 获取Context绑定的Handler
        H *getHandler() {
            return handler_.get();
        }

        // Context初始化
        void initialize(std::weak_ptr<PipelineBase> pipeline, std::shared_ptr<H> handler) {
            pipelineWeak_ = pipeline;
            pipelineRaw_ = pipeline.lock().get();//裸指针
            handler_ = std::move(handler);
        }

        // PipelineContext overrides
        void attachPipeline() override {
            // 如果该Context还没有被绑定
            if (!attached_) {
                this->attachContext(handler_.get(), impl_);// 将该Context绑定到handler上
                handler_->attachPipeline(impl_); // 调用Handler的attachPipeline，有具体的Handler实现
                attached_ = true;//标记Context已经attached到一个pipeline中
            }
        }

        // 从pipeline中分离
        void detachPipeline() override {
            handler_->detachPipeline(impl_);// 调用Handler的detachPipeline，有具体的Handler实现
            // 依附标志位为false
            attached_ = false;
        }

        void setNextIn(PipelineContext *ctx) override {
            if (!ctx) {
                nextIn_ = nullptr;
                return;
            }

            // 转成InboundLink，因为Context是InboundLink子类
            auto nextIn = dynamic_cast<InboundLink<typename H::rout> *>(ctx);
            if (nextIn) {
                nextIn_ = nextIn;
            } else {
                throw std::invalid_argument(folly::sformat(
                        "inbound type mismatch after {}", folly::demangle(typeid(H))));
            }
        }

        void setNextOut(PipelineContext *ctx) override {
            if (!ctx) {
                nextOut_ = nullptr;
                return;
            }
            auto nextOut = dynamic_cast<OutboundLink<typename H::wout> *>(ctx);
            if (nextOut) {
                nextOut_ = nextOut;
            } else {
                throw std::invalid_argument(folly::sformat(
                        "outbound type mismatch after {}", folly::demangle(typeid(H))));
            }
        }

        // 获取Context的方向
        HandlerDir getDirection() override {
            return H::dir;
        }

    protected:
        Context *impl_;                                    // 具体的Context实现
        std::weak_ptr<PipelineBase> pipelineWeak_;         //
        PipelineBase *pipelineRaw_;                        // 该Context绑定的pipeline
        std::shared_ptr<H> handler_;                       // 该Context包含的Handler
        InboundLink<typename H::rout> *nextIn_{nullptr};   // 下一个inbound类型的Context地址
        OutboundLink<typename H::wout> *nextOut_{nullptr}; // 下一个outbound类型的Context地址

    private:
        bool attached_{false}; // 这个Context是否已经被绑定
    };

// 具体的Context实现，HandlerContext：定义了inbound和outbound类型事件传递接口（事件传递接口都以fire为前缀）；
// InboundLink和OutboundLink主要用来作为链表管理inbound和outbound类型的Context;
    template<class H>
    class ContextImpl
            : public HandlerContext<typename H::rout, typename H::wout>,
              public InboundLink<typename H::rin>,
              public OutboundLink<typename H::win>,
              public ContextImplBase<H, HandlerContext < typename H::rout, typename H::wout>>

{
    public:
    typedef typename H::rin Rin;
    typedef typename H::rout Rout;
    typedef typename H::win Win;
    typedef typename H::wout Wout;
    static const HandlerDir dir = HandlerDir::BOTH;

    explicit ContextImpl(
            std::weak_ptr<PipelineBase> pipeline,
            std::shared_ptr<H> handler) {
    this->impl_ = this;//实现就是自己
    this->initialize(pipeline, std::move(handler));//初始化
}

// For StaticPipeline
ContextImpl() {
    this->impl_ = this;
}

~ContextImpl() = default;

// HandlerContext overrides
// Inbound类型的事件：read事件
void fireRead(Rout msg) override {
    auto guard = this->pipelineWeak_.lock();// 锁住，确保一旦锁住成功，在操作期间，pipeline不会被销毁
    // 如果还没有到最后
    if (this->nextIn_) {
        //  将事件继续向下传播（传给下一个Inbound类型的Context）
        //  注意：这里调用的是下一个Contex的read而不是fireRead
        //  即调用下一个Context里面的Handler方法
        this->nextIn_->read(std::forward<Rout>(msg));
    } else {
        LOG(WARNING) << "read reached end of pipeline";
    }
}

void fireReadEOF() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->readEOF();
    } else {
        LOG(WARNING) << "readEOF reached end of pipeline";
    }
}

void fireReadException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->readException(std::move(e));
    } else {
        LOG(WARNING) << "readException reached end of pipeline";
    }
}

void fireTransportActive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->transportActive();
    }
}

void fireTransportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->transportInactive();
    }
}

//Outbound类型的事件传播
folly::Future<folly::Unit> fireWrite(Wout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->write(std::forward<Wout>(msg));
    } else {
        LOG(WARNING) << "write reached end of pipeline";
        // 如果到了最后，返回一个future
        return folly::makeFuture();
    }
}

folly::Future<folly::Unit> fireWriteException(
        folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->writeException(std::move(e));
    } else {
        LOG(WARNING) << "close reached end of pipeline";
        return folly::makeFuture();
    }
}

folly::Future<folly::Unit> fireClose() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->close();
    } else {
        LOG(WARNING) << "close reached end of pipeline";
        return folly::makeFuture();
    }
}

// 获取Context绑定的pipeline指针
PipelineBase *getPipeline() override {
    return this->pipelineRaw_;
}

// 获取Context绑定的pipeline引用
std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
}

// 设置和获取wirte标志位
void setWriteFlags(folly::WriteFlags flags) override {
    this->pipelineRaw_->setWriteFlags(flags);
}

folly::WriteFlags getWriteFlags() override {
    return this->pipelineRaw_->getWriteFlags();
}

// 设置read缓冲区参数 minAvailable、allocationSize
void setReadBufferSettings(
        uint64_t minAvailable,
        uint64_t allocationSize) override {
    this->pipelineRaw_->setReadBufferSettings(minAvailable, allocationSize);
}

std::pair<uint64_t, uint64_t> getReadBufferSettings() override {
    return this->pipelineRaw_->getReadBufferSettings();
}

// InboundLink overrides
void read(Rin msg) override {
    // 保证pipeline不会被删除
    auto guard = this->pipelineWeak_.lock();
    // 调用该Context绑定的Handler的read方法，至于事件是都需要继续传播，完全受read中的实现
    this->handler_->read(this, std::forward<Rin>(msg));
}

void readEOF() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readEOF(this);
}

void readException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readException(this, std::move(e));
}

void transportActive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportActive(this);
}

void transportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportInactive(this);
}

// OutboundLink overrides
folly::Future<folly::Unit> write(Win msg) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->write(this, std::forward<Win>(msg));
}

folly::Future<folly::Unit> writeException(
        folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->writeException(this, std::move(e));
}

folly::Future<folly::Unit> close() override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->close(this);
}

};

template<class H>
class InboundContextImpl
        : public InboundHandlerContext<typename H::rout>,
          public InboundLink<typename H::rin>,
          public ContextImplBase<H, InboundHandlerContext < typename H::rout>>

{
public:
typedef typename H::rin Rin;
typedef typename H::rout Rout;
typedef typename H::win Win;
typedef typename H::wout Wout;
static const HandlerDir dir = HandlerDir::IN;

explicit InboundContextImpl(
        std::weak_ptr<PipelineBase>
pipeline,
std::shared_ptr<H> handler
) {
this->
impl_ = this;
this->
initialize(pipeline, std::move(handler)
);
}

// For StaticPipeline
InboundContextImpl() {
    this->impl_ = this;
}

~InboundContextImpl() = default;

// InboundHandlerContext overrides
void fireRead(Rout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->read(std::forward<Rout>(msg));
    } else {
        LOG(WARNING) << "read reached end of pipeline";
    }
}

void fireReadEOF() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->readEOF();
    } else {
        LOG(WARNING) << "readEOF reached end of pipeline";
    }
}

void fireReadException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->readException(std::move(e));
    } else {
        LOG(WARNING) << "readException reached end of pipeline";
    }
}

void fireTransportActive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->transportActive();
    }
}

void fireTransportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
        this->nextIn_->transportInactive();
    }
}

PipelineBase *getPipeline() override {
    return this->pipelineRaw_;
}

std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
}

// InboundLink overrides
void read(Rin msg) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->read(this, std::forward<Rin>(msg));
}

void readEOF() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readEOF(this);
}

void readException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readException(this, std::move(e));
}

void transportActive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportActive(this);
}

void transportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportInactive(this);
}

};

template<class H>
class OutboundContextImpl
        : public OutboundHandlerContext<typename H::wout>,
          public OutboundLink<typename H::win>,
          public ContextImplBase<H, OutboundHandlerContext < typename H::wout>>

{
public:
typedef typename H::rin Rin;
typedef typename H::rout Rout;
typedef typename H::win Win;
typedef typename H::wout Wout;
static const HandlerDir dir = HandlerDir::OUT;

explicit OutboundContextImpl(
        std::weak_ptr<PipelineBase>
pipeline,
std::shared_ptr<H> handler
) {
this->
impl_ = this;
this->
initialize(pipeline, std::move(handler)
);
}

// For StaticPipeline
OutboundContextImpl() {
    this->impl_ = this;
}

~OutboundContextImpl() = default;

// OutboundHandlerContext overrides
folly::Future<folly::Unit> fireWrite(Wout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->write(std::forward<Wout>(msg));
    } else {
        LOG(WARNING) << "write reached end of pipeline";
        return folly::makeFuture();
    }
}

folly::Future<folly::Unit> fireWriteException(
        folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->writeException(std::move(e));
    } else {
        LOG(WARNING) << "close reached end of pipeline";
        return folly::makeFuture();
    }
}

folly::Future<folly::Unit> fireClose() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
        return this->nextOut_->close();
    } else {
        LOG(WARNING) << "close reached end of pipeline";
        return folly::makeFuture();
    }
}

PipelineBase *getPipeline() override {
    return this->pipelineRaw_;
}

std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
}

// OutboundLink overrides
folly::Future<folly::Unit> write(Win msg) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->write(this, std::forward<Win>(msg));
}

folly::Future<folly::Unit> writeException(
        folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->writeException(this, std::move(e));
}

folly::Future<folly::Unit> close() override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->close(this);
}

};


// Context的类型依赖于Handler的类型
template<class Handler>
struct ContextType {
    // template< bool B, class T, class F >
    // type T if B == true, F if B == false
    typedef typename std::conditional<
            Handler::dir == HandlerDir::BOTH,//如果是双向
            ContextImpl < Handler>,          //类型就是ContextImpl<Handler>
    typename std::conditional<               //如果不是双向，那么还需要细分
            Handler::dir == HandlerDir::IN,  //如果是IN类型
            InboundContextImpl<Handler>,     //那么类型就是InboundContextImpl<Handler>
            OutboundContextImpl<Handler>     //否则就是OutboundContextImpl<Handler>
    >::type >::type
            type;                            // Context类型
};

} // namespace wangle
