#include "util/atomic.hpp"

#include "memory/header.hpp"

#include "bug.hpp"
#include "configuration.hpp"
#include "memory.hpp"
#include "on_stack.hpp"
#include "thread_phase.hpp"

#include "class/object.hpp"
#include "class/class.hpp"
#include "class/exception.hpp"
#include "class/fixnum.hpp"
#include "class/thread.hpp"

#include "diagnostics/memory.hpp"

#include <assert.h>
#include <sys/time.h>

#include <chrono>

namespace rubinius {
  std::atomic<uintptr_t> MemoryHeader::object_id_counter;

  void MemoryHeader::bootstrap(STATE) {
    object_id_counter = 1;
  }

  void MemoryHeader::track_reference(STATE) {
    if(reference_p()) {
      state->memory()->track_reference(this);
    }
  }

  void MemoryHeader::untrack_reference(STATE) {
    if(reference_p()) {
      state->memory()->untrack_reference(this);
    }
  }

  MemoryHeaderBits::MemoryHeaderBits(const MemoryHeader* header) {
    extended = header->extended_header_p();
    forwarded = header->forwarded_p();
    thread_id = header->thread_id();
    region = header->region();
    referenced = header->referenced();
    weakref = header->weakref_p();
    finalizer = header->finalizer_p();
    remembered = header->remembered_p();
    pinned = header->pinned_p();
    visited = header->visited();
    marked = header->marked();
    scanned = header->scanned_p();
    type_id = header->type_id();
    data = header->data_p();
    frozen = header->frozen_p();
    tainted = header->tainted_p();
    locked_count = header->lock_count();
    lock_extended = header->lock_extended_p();
    lock_owner = header->lock_owner();
    object_id = header->object_id();
    type_specific = header->type_specific();
  }

  MemoryHeaderBits MemoryHeader::header_bits() const {
    return MemoryHeaderBits(this);
  }

  bool MemoryLock::locked_p(STATE) {
    return locked_count > 0;
  }

  bool MemoryLock::lock_owned_p(STATE) {
    return thread_id == state->vm()->thread_id() && locked_count > 0;
  }

  bool MemoryLock::try_lock(STATE) {
    if(lock.try_lock()) {
      thread_id = state->vm()->thread_id();
      locked_count = 1;
      return true;
    } else {
      return false;
    }
  }

  void MemoryLock::unlock(STATE) {
    if(thread_id == state->vm()->thread_id()) {
      if(locked_count > 0) {
        locked_count--;

        if(locked_count == 0) {
          thread_id = 0;
          lock.unlock();
        }
      } else {
        Exception::raise_runtime_error(state,
            "unlocking owned, extended-header non-locked object");
      }
    } else {
      Exception::raise_runtime_error(state,
          "unlocking non-owned, extended-header object");
    }
  }

  ExtendedHeader* ExtendedHeader::create_lock(STATE, const MemoryFlags h, unsigned int count) {
    ExtendedHeader* nh = create(h);

    MemoryLock* lock = new MemoryLock(state->vm()->thread_id(), count);
    nh->words[0].set_lock(lock);

    return nh;
  }

  ExtendedHeader* ExtendedHeader::create_lock(
      STATE, const MemoryFlags h, const ExtendedHeader* eh, unsigned int count)
  {
    ExtendedHeader* nh = create(h, eh);

    MemoryLock* lock = new MemoryLock(state->vm()->thread_id(), count);
    nh->words[eh->size()].set_lock(lock);

    return nh;
  }

  ExtendedHeader* ExtendedHeader::replace_lock(STATE, const MemoryFlags h,
      const ExtendedHeader* eh, unsigned int count)
  {
    ExtendedHeader* nh = create_copy(h, eh);

    MemoryLock* lock = new MemoryLock(state->vm()->thread_id(), count);

    for(int i = 0; i < nh->size(); i++) {
      if(nh->words[i].lock_p()) {
        nh->words[i].set_lock(lock);
      }
    }

    return nh;
  }

  void MemoryHeader::lock(STATE) {
    static int i = 0;
    static int delay[] = {
      133, 464, 254, 306, 549, 287, 358, 638, 496, 81,
      472, 288, 131, 31, 435, 258, 221, 73, 537, 854
    };
    static int modulo = sizeof(delay) / sizeof(int);

    while(!try_lock(state)) {
      state->vm()->thread()->sleep(cTrue);

      uint64_t ns = delay[i++ % modulo];

      // TODO: MemoryHeader standardize this for waiting
      std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }

    state->vm()->thread()->sleep(cFalse);
  }

  bool MemoryHeader::try_lock(STATE) {
    while(true) {
      MemoryFlags h = header.load(std::memory_order_acquire);
      MemoryFlags nh;
      ExtendedHeader* eh = nullptr;

      if(thread_id() == state->vm()->thread_id()) {
        if(extended_header_p()) {
          ExtendedHeader* hh = extended_header();
          int locked_count = locked_count_field.get(hh->header);

          if(locked_count < max_locked_count()) {
            eh = ExtendedHeader::create_copy(
                locked_count_field.set(hh->header, locked_count + 1), hh);

            nh = extended_flags(eh);
          } else if(hh->lock_extended_p()) {
            MemoryLock* lock = hh->get_lock();

            if(lock->lock_owned_p(state)) {
              lock->locked_count++;
              return true;
            }

            if(lock->try_lock(state)) return true;

            if(state->vm()->thread_nexus()->valid_thread_p(state, lock->thread_id)) {
              return false;
            }

            eh = ExtendedHeader::replace_lock(state,
                locked_count_field.set(hh->header, lock_extended()), hh, 1);
            nh = extended_flags(eh);
          } else {
            eh = ExtendedHeader::create_lock(state,
                locked_count_field.set(hh->header, lock_extended()), hh, locked_count + 1);

            nh = extended_flags(eh);
          }
        } else {
          int locked_count = locked_count_field.get(h);

          if(locked_count < max_locked_count()) {
            nh = locked_count_field.set(h, locked_count + 1);
          } else {
            ExtendedHeader* eh = ExtendedHeader::create_lock(
                state, locked_count_field.set(h, lock_extended()), locked_count + 1);

            nh = extended_flags(eh);
          }
        }

        if(header.compare_exchange_strong(h, nh, std::memory_order_release)) {
          return true;
        }

        if(eh) eh->delete_header();
      } else {
        if(extended_header_p()) {
          ExtendedHeader* hh = extended_header();

          if(hh->lock_extended_p()) {
            MemoryLock* lock = hh->get_lock();

            if(lock->lock_owned_p(state)) {
              lock->locked_count++;
              return true;
            }

            if(lock->try_lock(state)) return true;

            if(state->vm()->thread_nexus()->valid_thread_p(state, lock->thread_id)) {
              return false;
            }

            eh = ExtendedHeader::replace_lock(
                state, locked_count_field.set(hh->header, lock_extended()), hh, 1);
            nh = extended_flags(eh);
          } else if(locked_count_field.get(hh->header) == 0) {
            eh = ExtendedHeader::create_lock(
                state, locked_count_field.set(hh->header, lock_extended()), hh, 1);
            nh = extended_flags(eh);
          } else {
            if(state->vm()->thread_nexus()->valid_thread_p(
                  state, thread_id_field.get(hh->header)))
            {
              return false;
            }

            eh = ExtendedHeader::create_lock(
                state, locked_count_field.set(hh->header, lock_extended()), hh, 1);
            nh = extended_flags(eh);
          }
        } else {
          if(locked_count_field.get(h) == 0) {
            ExtendedHeader* eh = ExtendedHeader::create_lock(
                state, locked_count_field.set(h, lock_extended()), 1);

            nh = extended_flags(eh);
          } else {
            if(state->vm()->thread_nexus()->valid_thread_p(
                  state, thread_id_field.get(h)))
            {
              return false;
            }

            eh = ExtendedHeader::create_lock(
                state, locked_count_field.set(h, lock_extended()), 1);
            nh = extended_flags(eh);
          }
        }

        if(header.compare_exchange_strong(h, nh, std::memory_order_release)) {
          return true;
        }

        if(eh) eh->delete_header();
      }
    }
  }

  void MemoryHeader::unlock(STATE) {
    while(true) {
      MemoryFlags h = header.load(std::memory_order_acquire);

      if(thread_id() == state->vm()->thread_id()) {
        if(extended_header_p()) {
          ExtendedHeader* hh = extended_header();

          if(hh->lock_extended_p()) {
            hh->get_lock()->unlock(state);
            return;
          } else {
            int locked_count = locked_count_field.get(hh->header);

            if(locked_count > 0) {
              ExtendedHeader* eh = ExtendedHeader::create_copy(
                  locked_count_field.set(hh->header, locked_count - 1), hh);

              MemoryFlags nh = extended_flags(eh);

              if(header.compare_exchange_strong(h, nh, std::memory_order_release)) {
                return;
              }

              eh->delete_header();
            } else {
              Exception::raise_runtime_error(state,
                  "unlocking owned, extended-header, non-locked object");
            }
          }
        } else {
          int locked_count = locked_count_field.get(h);

          if(locked_count > 0) {
            MemoryFlags nh = locked_count_field.set(h, locked_count - 1);

            if(header.compare_exchange_strong(h, nh, std::memory_order_release)) {
              return;
            }
          } else {
            Exception::raise_runtime_error(state, "unlocking owned, non-locked object");
          }
        }
      } else {
        if(extended_header_p()) {
          ExtendedHeader* hh = extended_header();

          if(hh->lock_extended_p()) {
            hh->get_lock()->unlock(state);
            return;
          } else {
            Exception::raise_runtime_error(state,
                "unlocking non-owned, extended-header, non-locked object");
          }
        } else {
          Exception::raise_runtime_error(state, "unlocking non-owned, non-locked object");
        }
      }
    }
  }

  void MemoryHeader::unset_lock(STATE) {
    unset(locked_count_field);
  }

  bool MemoryHeader::locked_p(STATE) {
    if(extended_header_p()) {
      ExtendedHeader* hh = extended_header();

      if(hh->lock_extended_p()) {
        return hh->get_lock()->locked_p(state);
      } else {
        return locked_count_field.get(hh->header) > 0;
      }
    } else {
      return locked_count_field.get(header) > 0;
    }
  }

  bool MemoryHeader::lock_owned_p(STATE) {
    return false;
  }

  size_t ObjectHeader::compute_size_in_bytes(VM* vm) const {
    return vm->memory()->type_info[type_id()]->object_size(this);
  }

  size_t DataHeader::compute_size_in_bytes(VM* vm) const {
    return vm->memory()->type_info[type_id()]->object_size(this);
  }

  void ObjectHeader::initialize_copy(STATE, Object* other) {
    klass(other->klass());
    ivars(other->ivars());

    state->shared().om->write_barrier(this, klass());
    state->shared().om->write_barrier(this, ivars());
  }

  void ObjectHeader::copy_body(VM* state, Object* other) {
    void* src = other->__body__;
    void* dst = this->__body__;

    memcpy(dst, src, other->body_in_bytes(state));
  }
}
