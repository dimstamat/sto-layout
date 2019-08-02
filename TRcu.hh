#pragma once
#include "compiler.hh"
#include <assert.h>

struct TRcuGroup {
    typedef uint64_t epoch_type;
    typedef int64_t signed_epoch_type;

    struct TRcuElement {
        void (*function)(void*);
        union {
            void* argument;
            epoch_type epoch;
        } u;
    };

    unsigned head_;
    unsigned tail_;
    unsigned capacity_;
    epoch_type epoch_;
    TRcuGroup* next_;
    TRcuElement e_[1];

private:
    TRcuGroup(unsigned capacity)
        : head_(0), tail_(0), capacity_(capacity), next_(nullptr) {
    }
    TRcuGroup(const TRcuGroup&) = delete;
    ~TRcuGroup() {
        while (head_ != tail_) {
            if (e_[head_].function)
            //Dimos, segfault in some runs when calling free!
            //if (e_[head_].function && e_[head_].u.argument)
                e_[head_].function(e_[head_].u.argument);
            ++head_;
        }
    }

public:
    static TRcuGroup* make(unsigned capacity) {
        void* x = new char[sizeof(TRcuGroup) + sizeof(TRcuElement) * (capacity - 1)];
        return new(x) TRcuGroup(capacity);
    }
    static void free(TRcuGroup* g) {
        g->~TRcuGroup();
        delete[] reinterpret_cast<char*>(g);
    }

    void add(epoch_type epoch, void (*function)(void*), void* argument) {
        assert(tail_ + 2 <= capacity_);
        if (head_ == tail_ || epoch_ != epoch) {
            e_[tail_].function = nullptr;
            e_[tail_].u.epoch = epoch;
            epoch_ = epoch;
            ++tail_;
        }
        e_[tail_].function = function;
        e_[tail_].u.argument = argument;
        ++tail_;
    }
    inline bool clean_until(epoch_type max_epoch);
};

class TRcuSet {
public:
    typedef TRcuGroup::epoch_type epoch_type;
    typedef TRcuGroup::signed_epoch_type signed_epoch_type;

    TRcuSet();
    ~TRcuSet();

    void add(epoch_type epoch, void (*function)(void*), void* argument) {
        if (unlikely(current_->tail_ + 2 > current_->capacity_))
            grow();
        current_->add(epoch, function, argument);
    }
    void clean_until(epoch_type max_epoch) {
        if (clean_epoch_ != max_epoch)
            hard_clean_until(max_epoch);
        clean_epoch_ = max_epoch;
    }
    epoch_type clean_epoch() const {
        return clean_epoch_;
    }

private:
    TRcuGroup* current_;
    TRcuGroup* first_;
    epoch_type clean_epoch_;
    // unsigned ngroups_;

    TRcuSet(const TRcuSet&) = delete;
    TRcuSet& operator=(const TRcuSet&) = delete;
    void check();
    void grow();
    void hard_clean_until(epoch_type max_epoch);
};
