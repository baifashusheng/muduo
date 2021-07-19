// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_EVENTLOOP_H
#define MUDUO_NET_EVENTLOOP_H

#include <atomic>
#include <functional>
#include <vector>

#include <boost/any.hpp>

#include "muduo/base/Mutex.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Timestamp.h"
#include "muduo/net/Callbacks.h"
#include "muduo/net/TimerId.h"

namespace muduo
{
namespace net
{

class Channel;
class Poller;
class TimerQueue;

///
/// Reactor, at most one per thread.
///
/// This is an interface class, so don't expose too much details.
//事件循环类 主要包含了三大模块 Channel 通道模块 和 Poller模块（epoll的抽象类）和 TimerQueue,这两个模块相当于在多路分发器里面的操作
//一个EventLoop包含很多Channel和一个Poller 、TimerQueue

class EventLoop : noncopyable
{
 public:
  typedef std::function<void()> Functor;

  EventLoop();
  ~EventLoop();  // force out-line dtor, for std::unique_ptr members.

  ///
  /// Loops forever.
  ///
  /// Must be called in the same thread as creation of the object.
  //开启事件循环
  void loop();

  /// Quits loop.
  ///退出事件循环
  /// This is not 100% thread safe, if you call through a raw pointer,
  /// better to call through shared_ptr<EventLoop> for 100% safety.
  void quit();

  ///
  /// Time when poll returns, usually means data arrival.
  ///
  Timestamp pollReturnTime() const { return pollReturnTime_; }

  int64_t iteration() const { return iteration_; }

  /// Runs callback immediately in the loop thread.
  /// It wakes up the loop, and run the cb.
  /// If in the same loop thread, cb is run within the function.
  /// Safe to call from other threads.
  //在当前loop中执行cb
  void runInLoop(Functor cb);
  /// Queues callback in the loop thread.
  /// Runs after finish pooling.
  /// Safe to call from other threads.
  //把cb放入队列中，唤醒loop所在的线程，执行cb
  void queueInLoop(Functor cb);

  size_t queueSize() const;

  // timers

  ///
  /// Runs callback at 'time'.
  /// Safe to call from other threads.
  ///
  TimerId runAt(Timestamp time, TimerCallback cb);
  ///
  /// Runs callback after @c delay seconds.
  /// Safe to call from other threads.
  ///
  TimerId runAfter(double delay, TimerCallback cb);
  ///
  /// Runs callback every @c interval seconds.
  /// Safe to call from other threads.
  ///
  TimerId runEvery(double interval, TimerCallback cb);
  ///
  /// Cancels the timer.
  /// Safe to call from other threads.
  ///
  void cancel(TimerId timerId);

  // internal usage
  //用来唤醒loop所在线程的
  void wakeup();
    //EventLoop的方法 =》 Poller的方法
  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);
  bool hasChannel(Channel* channel);

  // pid_t threadId() const { return threadId_; }
  void assertInLoopThread()
  {
    if (!isInLoopThread())
    {
      abortNotInLoopThread();
    }
  }

    //判断EventLoop对象是否在自己的线程里面
  bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
  // bool callingPendingFunctors() const { return callingPendingFunctors_; }
  bool eventHandling() const { return eventHandling_; }

  void setContext(const boost::any& context)
  { context_ = context; }

  const boost::any& getContext() const
  { return context_; }

  boost::any* getMutableContext()
  { return &context_; }

  static EventLoop* getEventLoopOfCurrentThread();

 private:
  void abortNotInLoopThread();
  void handleRead();  // waked up
  void doPendingFunctors();//执行回调

  void printActiveChannels() const; // DEBUG

  typedef std::vector<Channel*> ChannelList;

  bool looping_; /* atomic 判断是否开始事件循环 */
  std::atomic<bool> quit_; //标识退出loop循环
  bool eventHandling_; /* atomic */
  bool callingPendingFunctors_; /* atomic */
  int64_t iteration_;
  const pid_t threadId_; //记录当前loop所在线程的id  /* 创建时保存当前事件循环所在线程，用于之后运行时判断使用EventLoop的线程是否是EventLoop所属的线程 */
  Timestamp pollReturnTime_; //poller返回发生事件的channels的时间点  用于计算从激活到调用回调函数的延迟
  std::unique_ptr<Poller> poller_;/* io多路复用 */
  std::unique_ptr<TimerQueue> timerQueue_;/* 定时器队列 */
    /* 唤醒当前线程的描述符 */
  int wakeupFd_;//主要作用为，当mianLoop获取一个新用户的channel，通过轮询算法选择一个subloop，通过该成员唤醒subloop处理channel
  // unlike in TimerQueue, which is an internal class,
  // we don't expose Channel to client.
    /*
   * 用于唤醒当前线程，因为当前线程主要阻塞在poll函数上
   * 所以唤醒的方法就是手动激活这个wakeupChannel_，即写入几个字节让Channel变为可读
   * 注: 这个Channel也注册到Poller中
   */
  std::unique_ptr<Channel> wakeupChannel_;
  boost::any context_;

  // scratch variables
    /*
   * 激活队列，poll函数在返回前将所有激活的Channel添加到激活队列中
   * 在当前事件循环中的所有Channel在Poller中
   */
  ChannelList activeChannels_;
    /* 当前执行回调函数的Channel */
  Channel* currentActiveChannel_;

    /*
   * queueInLoop添加函数时给pendingFunctors_上锁，防止多个线程同时添加
   *
   * mutable,突破const限制，在被const声明的函数仍然可以更改这个变量
   */
  mutable MutexLock mutex_;

    /*
   * 等待在当前线程调用的回调函数，
   * 原因是本来属于当前线程的回调函数会被其他线程调用时，应该把这个回调函数添加到它属于的线程中
   * 等待它属于的线程被唤醒后调用，以满足线程安全性
   *
   * TcpServer::removeConnection是个例子
   * 当关闭一个TcpConnection时，需要调用TcpServer::removeConnection，但是这个函数属于TcpServer，
   * 然而TcpServer和TcpConnection不属于同一个线程，这就容易将TcpServer暴露给其他线程，
   * 万一其他线程析构了TcpServer怎么办（线程不安全）
   * 所以会调用EventLoop::runInLoop，如果要调用的函数属于当前线程，直接调用
   * 否则，就添加到这个队列中，等待当前线程被唤醒
   */

 /*
  * 它是一个任务容器，存放的是将要执行的回调函数。
准备这么一个容器的原因在于
某个对象（通常是Channel或者TcpConnection）可能被另一个线程使用（这个线程不是这个对象所在线程），
  此时这个对象就等于暴露给其他线程了。这是非常不安全的，万一这个线程不小心析构了这个对象，
  而这个对象所属的那个线程正要访问这个对象（例如调用这个对象的接口），这个线程就会崩溃，因为它访问了一个本不存在的对象（已经被析构）。
为了解决这个问题，就需要尽量将对这个对象的操作移到它所属的那个线程执行（这里是调用这个对象的接口）以满足线程安全性。
  又因为每个对象都有它所属的事件驱动循环EventLoop，这个EventLoop通常阻塞在poll上。
  可以保证的是EventLoop阻塞的线程就是它所属的那个线程，所以调用poll的线程就是这个对象所属的线程。
  这就 可以让poll返回后再执行想要调用的函数，但是需要手动唤醒poll，否则一直阻塞在那里会耽误函数的执行。
  *
  * */
  std::vector<Functor> pendingFunctors_ GUARDED_BY(mutex_);//存储loop需要执行的所有的回调操作
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_EVENTLOOP_H
