/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <condition_variable>
#include <forward_list>
#include <mutex>
#include <thread>

#include <rex/assert.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

// Timers are handed to a single dispatch thread, which blocks on a condition
// variable until a new timer arrives or the earliest pending one is due. It
// does not wake up while there is nothing to do.
//
// This used to be a disruptorplus ring buffer with a spin-wait strategy, which
// kept yielding and sleeping for 1 ms in a loop even with no timer queued: in
// the order of a thousand wake-ups per second competing with the guest, which
// matters on machines with few cores.
class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue() {
    dispatch_thread_ = std::thread([this] { TimerThreadMain(); });
  }

  ~TimerQueue() {
    // Stop and join before the state used by the dispatch thread is destroyed.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
  }

  void TimerThreadMain() {
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };
    const auto has_pending = [this] { return !pending_.empty(); };

    set_current_thread_name("rex::thread::TimerQueue");

    // This is a _sorted_ (ascending due_) list of active timers. Only this
    // thread touches it, so it needs no lock.
    std::forward_list<std::shared_ptr<WaitItem>> wait_queue;

    while (true) {
      {
        // Wait for new wait items or for the earliest one to be due, then take
        // the new ones and add them to the sorted wait queue
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          if (pending_.empty()) {
            if (wait_queue.empty()) {
              cv_.wait(lock, [this, &has_pending] { return stopping_ || has_pending(); });
            } else {
              // Bounded so a far-future due time never reaches the clock
              // conversions inside the wait.
              const auto limit = clock::now() + std::chrono::hours(1);
              cv_.wait_until(lock, std::min(wait_queue.front()->due_, limit),
                             [this, &has_pending] { return stopping_ || has_pending(); });
            }
          }
          if (stopping_) {
            break;
          }
          wait_items.swap(pending_);
        }
        wait_items.sort(comp);
        wait_queue.merge(wait_items, comp);
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue.empty() && wait_queue.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue.front());
          wait_queue.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_front(std::move(wait_item));
    }
    cv_.notify_one();

    return wait_item_weak;
  }

  std::thread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // Wait items queued by the public API and not yet taken by the dispatch
  // thread. Guarded by mutex_.
  std::mutex mutex_;
  std::condition_variable cv_;
  std::forward_list<std::shared_ptr<WaitItem>> pending_;
  bool stopping_ = false;

  // Declared last: it is stopped and joined before the members above go away.
  std::thread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
