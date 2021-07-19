// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
/*
 * muduo的设计采用高并发服务器框架中的one loop per thread模式，即一个线程一个事件循环。
这里的loop，其实就是muduo中的EventLoop，所以到目前为止，
 不管是Poller，Channel还是TimerQueue都仅仅是单线程下的任务，因为这些都依赖于EventLoop。
 这每一个EventLoop，其实也就是一个Reactor模型。
而多线程体现在EventLoop的上层，即在EventLoop上层有一个线程池，
 线程池中每一个线程运行一个EventLoop，也就是Reactor + 线程池的设计模式
 * */



#include "muduo/net/EventLoop.h"

#include "muduo/base/Logging.h"
#include "muduo/base/Mutex.h"
#include "muduo/net/Channel.h"
#include "muduo/net/Poller.h"
#include "muduo/net/SocketsOps.h"
#include "muduo/net/TimerQueue.h"

#include <algorithm>

#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

namespace
{
    // 防止一个线程创建多个EventLoop   thread_local
__thread EventLoop* t_loopInThisThread = 0;
// 定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000;
// 创建wakeupfd，用来notify唤醒subReactor处理新来的channel
int createEventfd()
{
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    LOG_SYSERR << "Failed in eventfd";
    abort();
  }
  return evtfd;
}

#pragma GCC diagnostic ignored "-Wold-style-cast"
class IgnoreSigPipe
{
 public:
  IgnoreSigPipe()
  {
    ::signal(SIGPIPE, SIG_IGN);
    // LOG_TRACE << "Ignore SIGPIPE";
  }
};
#pragma GCC diagnostic error "-Wold-style-cast"

IgnoreSigPipe initObj;
}  // namespace

EventLoop* EventLoop::getEventLoopOfCurrentThread()
{
  return t_loopInThisThread;
}

EventLoop::EventLoop()
  : looping_(false),
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false),
    iteration_(0),
    threadId_(CurrentThread::tid()),
    poller_(Poller::newDefaultPoller(this)),
    timerQueue_(new TimerQueue(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(new Channel(this, wakeupFd_)),
    currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this;
  }

  // 设置wakeupfd的事件类型以及发生事件后的回调操作
  wakeupChannel_->setReadCallback(
      std::bind(&EventLoop::handleRead, this));
  // we are always reading the wakeupfd
  // 每一个eventloop都将监听wakeupchannel的EPOLLIN读事件了
  wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;
}

// 开启事件循环
void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();
  looping_ = true;
  quit_ = false;  // FIXME: what if someone calls quit() before loop() ?
  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();
      // 监听两类fd   一种是client的fd，一种wakeupfd
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
    ++iteration_;
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();
    }
    // TODO sort channel by priority
    eventHandling_ = true;
    for (Channel* channel : activeChannels_)
    {
      currentActiveChannel_ = channel;
      // Poller监听哪些channel发生事件了，然后上报给EventLoop，通知channel处理相应的事件
      currentActiveChannel_->handleEvent(pollReturnTime_);
    }
    currentActiveChannel_ = NULL;
    eventHandling_ = false;
      // 执行当前EventLoop事件循环需要处理的回调操作
      /**
       * IO线程 mainLoop accept fd《=channel subloop
       * mainLoop 事先注册一个回调cb（需要subloop来执行）    wakeup subloop后，执行下面的方法，执行之前mainloop注册的cb操作
       */
    doPendingFunctors();
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

void EventLoop::quit()
{
  quit_ = true;
  // 如果是在其它线程中，调用的quit   在一个subloop(woker)中，调用了mainLoop(IO)的quit
  // There is a chance that loop() just executes while(!quit_) and exits,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())
  {
    wakeup();
  }
}

// 在当前loop中执行cb
/*
 * 1.如果事件循环不属于当前这个线程，就不能直接调用回调函数，应该回到自己所在线程调用
 * 2.此时需要先添加到自己的队列中存起来，然后唤醒自己所在线程的io复用函数（poll）
 * 3.唤醒方法是采用eventfd，这个eventfd只有8字节的缓冲区，向eventfd中写入数据另poll返回
 * 4.返回后会调用在队列中的函数，见EventLoop
 *
 * 举例说明什么时候会出现事件驱动循环不属于当前线程的情况
 *      1.客户端close连接，服务器端某个Channel被激活，原因为EPOLLHUP
 *      2.Channel调用回调函数，即TcpConnection的handleClose
 *      3.handleClose调用TcpServer为它提供的回调函数removeConnection
 *      4.此时执行的是TcpServer的removeConnection函数，
 * 解释
 *      1.因为TcpServer所在线程和TcpConnection所在的不是同一个线程
 *      2.这就导致将TcpServer暴露给了TcpConnection所在线程
 *      3.因为TcpServer需要将这个关闭的TcpConnection从tcp map中删除
 *        就需要调用自己的另一个函数removeConnectionInLoop
 *      4.为了实现线程安全性，也就是为了让removeConnectionInLoop在TcpServer自己所在线程执行
 *        需要先把这个函数添加到队列中存起来，等到回到自己的线程在执行
 *      5.runInLoop中的queueInLoop就是将这个函数存起来
 *      6.而此时调用runInLoop的仍然是TcpConnection所在线程
 *      7.因为自始至终，removeConnection这个函数都还没有结束
 *
 * 如果调用runInLoop所在线程和事件驱动循环线程是一个线程，那么直接调用回调函数就行了
 *
 * 在TcpServer所在线程中，EventLoop明明阻塞在poll上，这里为什么可以对它进行修改
 *      1.线程相当于一个人可以同时做两件事情，一个EventLoop同时调用两个函数就很正常了
 *      2.其实函数调用都是通过函数地址调用的，既然EventLoop可读，就一定直到内部函数的地址，自然可以调用
 *      3.而更改成员函数，通过地址访问，进而修改，也是可以的
 */
/*在它的IO线程内执行某个用户任务回调，即EventLoop::runInLoop(Functor cb)，其中Functor是boost::function<void()>.
 * 如果用戶在当前IO线程调用这个函数，回调会同步进行，如果用户在其他线程调用runInLoop(),cb会被加入队列，IO线程会被唤醒来调用这个Functor
 * 有了这个功能，我们就能轻易地在线程间调配任务，比如说把TimerQueen的成员函数调用移到其他IO线程，这样可以不用锁的情况下保证线程安全
 * IO 线程平时阻塞在事件循环 EventLoop::loop() 中 poll(2)调用中，为了能让 IO线程立刻执行新加入的用户回调，我们需要设法唤醒它。

传统的办法是用 pipe(2)，IO线程始终监视此管道的 readable 事件，在需要唤醒的时候，其他线程往管道里写一个字节，
 这样IO线程就从 IO multiplexing 阻塞调用中返回。（HTTP long polling?）
现在Linux有了 eventfd(2)，可以更高效地唤醒，因为它不必管理缓冲区。

 * */
void EventLoop::runInLoop(Functor cb)
{
  if (isInLoopThread())// 在当前的loop线程中，执行cb
  {
    cb();
  }
  else
  {
      // 在非当前loop线程中执行cb , 就需要唤醒loop所在线程，执行cb
    queueInLoop(std::move(cb));
  }
}

// 把cb放入队列中，唤醒loop所在的线程，执行cb
void EventLoop::queueInLoop(Functor cb)
{
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(std::move(cb));
  }
// 唤醒相应的，需要执行上面回调操作的loop的线程了
// || callingPendingFunctors_的意思是：当前loop正在执行回调，但是loop又有了新的回调
  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();// 唤醒loop所在线程
  }
}

size_t EventLoop::queueSize() const
{
  MutexLockGuard lock(mutex_);
  return pendingFunctors_.size();
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb)
{
  return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

//不是线程安全
TimerId EventLoop::runAfter(double delay, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);
}

void EventLoop::updateChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  if (eventHandling_)
  {
    assert(currentActiveChannel_ == channel ||
        std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
  }
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " <<  CurrentThread::tid();
}

// 用来唤醒loop所在的线程的  向wakeupfd_写一个数据，wakeupChannel就发生读事件，当前loop线程就会被唤醒
void EventLoop::wakeup()
{
  uint64_t one = 1;
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  {
  MutexLockGuard lock(mutex_);
  functors.swap(pendingFunctors_);
  }

  for (const Functor& functor : functors)
  {
    functor();
  }
  callingPendingFunctors_ = false;
}

void EventLoop::printActiveChannels() const
{
  for (const Channel* channel : activeChannels_)
  {
    LOG_TRACE << "{" << channel->reventsToString() << "} ";
  }
}

