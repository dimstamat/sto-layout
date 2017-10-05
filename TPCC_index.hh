#pragma once
#include "config.h"
#include "compiler.hh"

#include "Transaction.hh"
#include "TWrapped.hh"

#include "masstree.hh"
#include "kvthread.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_print.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "string.hh"

//#include "MassTrans.hh"

#include <vector>

namespace tpcc {

// unordered index implemented as hashtable
template <typename K, typename V,
          bool Opacity = true, bool Adaptive = false, bool ReadMyWrite = false>
class unordered_index : public TObject {
public:
    typedef K key_type;
    typedef V value_type;

    typedef typename std::conditional<Opacity, TVersion, TNonopaqueVersion>::type bucket_version_type;
    typedef typename std::conditional<Adaptive, TLockVersion, bucket_version_type>::type version_type;

    typedef std::hash<K> Hash;
    typedef std::equal_to<K> Pred;
    
    static constexpr typename version_type::type invalid_bit = TransactionTid::user_bit;

    static constexpr bool index_read_my_write = ReadMyWrite;

private:
    // our hashtable is an array of linked lists. 
    // an internal_elem is the node type for these linked lists
    struct internal_elem {
        internal_elem *next;
        key_type key;
        version_type version;
        value_type value;
        bool deleted;

        internal_elem(const key_type& k, const value_type& val, bool mark_valid)
            : next(nullptr), key(k),
              version(Sto::initialized_tid() | (mark_valid ? 0 : invalid_bit)),
              value(val), deleted(false) {}

        bool valid() const {
            return !(version.value() & invalid_bit);
        }
    };

    struct bucket_entry {
        internal_elem *head;
        // this is the bucket version number, which is incremented on insert
        // we use it to make sure that an unsuccessful key lookup will still be
        // unsuccessful at commit time (because this will always be true if no
        // new inserts have occurred in this bucket)
        bucket_version_type version;
        bucket_entry() : head(nullptr), version(0) {}
    };

    typedef std::vector<bucket_entry> MapType;
    // this is the hashtable itself, an array of bucket_entry's
    MapType map_;
    Hash hasher_;
    Pred pred_;

    uint64_t key_gen_;

    // used to mark whether a key is a bucket (for bucket version checks)
    // or a pointer (which will always have the lower 3 bits as 0)
    static constexpr uintptr_t bucket_bit = 1U<<0;

    static constexpr TransItem::flags_type insert_bit = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit<<1;

public:
    typedef std::tuple<bool, bool, uintptr_t, const value_type*> sel_return_type;
    typedef std::tuple<bool, bool>                               ins_return_type;
    typedef std::tuple<bool, bool>                               del_return_type;

    unordered_index(size_t size, Hash h = Hash(), Pred p = Pred()) :
            map_(), hasher_(h), pred_(p), key_gen_(0) {
        map_.resize(size);
    }

    inline size_t hash(const key_type& k) const {
        return hasher_(k);
    }
    inline size_t nbuckets() const {
        return map_.size();
    }
    inline size_t find_bucket_idx(const key_type& k) const {
        return hash(k) % nbuckets();
    }

    uint64_t gen_key() {
        return fetch_and_add(&key_gen_);
    }

    // returns (success : bool, found : bool, row_ptr : const internal_elem *)
    // will need to row back transaction if success == false
    sel_return_type
    select_row(const key_type& k, bool for_update = false) {
        bucket_entry& buck = map_[find_bucket_idx(k)];
        bucket_version_type buck_vers = buck.version;
        fence();
        internal_elem *e = find_in_bucket(buck, k);

        if (e) {
            // if found, return pointer to the row
            auto item = Sto::item(this, e);
            if (is_phantom(e, item))
                goto abort;

            if (index_read_my_write) {
                if (has_delete(item))
                    return sel_return_type(true, false, 0, nullptr);
                if (item.has_write())
                    return sel_return_type(true, true, reinterpret_cast<uintptr_t>(e), &((item.template write_value<internal_elem *>())->value));
            }

            if (for_update) {
                if (!select_for_update(item, e->version))
                    goto abort;
            } else {
                if (!item.observe(e->version))
                    goto abort;
            }

            return sel_return_type(true, true, reinterpret_cast<uintptr_t>(e), &e->value);
        } else {
            // if not found, observe bucket version
            bool ok = Sto::item(this, make_bucket_key(buck)).observe(buck_vers);
            if (!ok)
                goto abort;
            return sel_return_type(true, false, 0, nullptr);
        }

    abort:
        return sel_return_type(false, false, 0, nullptr);
    }

    // this method is only to be used after calling select_row() with for_update set to true
    // otherwise behavior is undefined
    // update_row() takes ownership of the row pointer (new_row) passed in, and the row to be updated (table_row)
    // should never be modified by the transaction user
    // the new_row pointer stays valid for the rest of the duration of the transaction and the associated
    // temporary row WILL NOT be deallocated until commit/abort time
    void update_row(uintptr_t rid, value_type *new_row) {
        auto item = Sto::item(this, reinterpret_cast<internal_elem *>(rid));
        assert(item.has_write() && !has_insert(item));
        item.add_write(new_row);
    }

    // returns (success : bool, found : bool)
    // insert_row() takes ownership of the row pointer (vptr) passed in
    // the pointer stays valid for the rest of the duration of the transaction and the associated temporary row
    // WILL NOT be deallocated until commit/abort time
    ins_return_type
    insert_row(const key_type& k, value_type *vptr, bool overwrite = false) {
        bucket_entry& buck = map_[find_bucket_idx(k)];

        lock(buck.version);
        internal_elem *e = find_in_bucket(buck, k);

        if (e) {
            unlock(buck.version);
            auto item = Sto::item(this, e);
            if (is_phantom(e, item))
                goto abort;

            if (index_read_my_write) {
                if (has_delete(item)) {
                    item.clear_flags(delete_bit).clear_write().template add_write<value_type *>(vptr);
                    return ins_return_type(true, false);
                }
            }

            // cope with concurrent deletion
            //if (!item.observe(e->version))
            //    goto abort;

            if (overwrite) {
                if (!select_for_overwrite(item, e->version, vptr))
                    goto abort;
                if (index_read_my_write) {
                    if (has_insert(item)) {
                        copy_row(e, vptr);
                    }
                }
            } else {
                if (!item.observe(e->version))
                    goto abort;
            }

            return ins_return_type(true, true);
        } else {
            // insert the new row to the table and take note of bucket version changes
            bucket_version_type buck_vers_0 = buck.version.unlocked();
            insert_in_bucket(buck, k, vptr, false);
            internal_elem *new_head = buck.head;
            bucket_version_type buck_vers_1 = buck.version.unlocked();

            unlock(buck.version);

            // update bucket version in the read set (if any) since it's changed by ourselves
            auto bucket_item = Sto::item(this, make_bucket_key(buck));
            if (bucket_item.has_read())
                bucket_item.update_read(buck_vers_0, buck_vers_1);

            auto item = Sto::item(this, new_head);
            // XXX adding write is probably unnecessary, am I right?
            item.template add_write<value_type *>(vptr);
            item.add_flags(insert_bit);

            return ins_return_type(true, false);
        }

    abort:
        return ins_return_type(false, false);
    }

    // returns (success : bool, found : bool)
    // for rows that are not inserted by this transaction, the actual delete doesn't take place
    // until commit time
    del_return_type
    delete_row(const key_type& k) {
        bucket_entry& buck = map_[find_bucket_idx(k)];
        bucket_version_type buck_vers = buck.version;
        fence();

        internal_elem *e = find_in_bucket(buck, k);
        if (e) {
            auto item = Sto::item(this, e);
            bool valid = e->valid();
            if (is_phantom(e, item))
                goto abort;
            if (index_read_my_write) {
                if (!valid && has_insert(item)) {
                    // deleting something we inserted
                    _remove(e);
                    item.remove_read().remove_write().clear_flags(insert_bit | delete_bit);
                    Sto::item(this, make_bucket_key(buck)).observe(buck_vers);
                    return del_return_type(true, true);
                }
                assert(valid);
                if (has_delete(item))
                    return del_return_type(true, false);
            }
            // select_for_update() will automatically add an observation for OCC version types
            // so that we can catch change in "deleted" status of a table row at commit time
            if (!select_for_update(item, e->version))
                goto abort;
            fence();
            // it vital that we check the "deleted" status after registering an observation
            if (e->deleted)
                goto abort;
            item.add_flags(delete_bit);

            return del_return_type(true, true);
        } else {
            // not found -- add observation of bucket version
            bool ok = Sto::item(this, make_bucket_key(buck)).observe(buck_vers);
            if (!ok)
                goto abort;
            return del_return_type(true, false);
        }

    abort:
        return del_return_type(false, false);
    }

    // TObject interface methods
    bool lock(TransItem& item, Transaction& txn) override {
        assert(!is_bucket(item));
        internal_elem *el = item.key<internal_elem *>();
        return txn.try_lock(item, el->version);
    }

    bool check(TransItem& item, Transaction&) override {
        if (is_bucket(item)) {
            bucket_entry &buck = *bucket_address(item);
            return buck.version.check_version(item.read_value<bucket_version_type>());
        } else {
            internal_elem *el = item.key<internal_elem *>();
            version_type rv = item.read_value<version_type>();
            return el->version.check_version(rv);
        }
    }

    void install(TransItem& item, Transaction& txn) override {
        assert(!is_bucket(item));
        internal_elem *el = item.key<internal_elem*>();
        if (has_delete(item)) {
            el->deleted = true;
            fence();
            el->version.set_version_locked(el->version.value() + TransactionTid::increment_value);
            return;
        }
        if (!has_insert(item)) {
            // update
            copy_row(el, item.write_value<const value_type *>());
        }
        el->version.set_version(txn.commit_tid());

        if (Opacity && has_insert(item)) {
            bucket_entry& buck = map_[find_bucket_idx(el->key)];
            lock(buck.version);
            if (buck.version.value() & TransactionTid::nonopaque_bit)
                buck.version.set_version(txn.commit_tid());
            unlock(buck.version);
        }
    }

    void unlock(TransItem& item) override {
        assert(!is_bucket(item));
        internal_elem *el = item.key<internal_elem *>();
        unlock(el->version);
    }

    void cleanup(TransItem& item, bool committed) override {
        if (committed ? has_delete(item) : has_insert(item)) {
            assert(!is_bucket(item));
            internal_elem *el = item.key<internal_elem *>();
            assert(!el->valid() || el->deleted);
            _remove(el);
        }
    }

    // non-transactional methods
    value_type* nontrans_get(const key_type& k) {
        bucket_entry& buck = map_[find_bucket_idx(k)];
        return find_in_bucket(k);
    }
    void nontrans_put(const key_type& k, const value_type& v) {
        bucket_entry& buck = map_[find_bucket_idx(k)];
        lock(buck.version);
        internal_elem *el = find_in_bucket(buck, k);
        if (el == nullptr) {
            internal_elem *new_head = new internal_elem(k, v, true);
            internal_elem *curr_head = buck.head;
            new_head->next = curr_head;
            buck.head = new_head;
        } else {
            copy_row(el, &v);
        }
        unlock(buck.version);
    }

private:
    // remove a k-v node during transactions (with locks)
    void _remove(internal_elem *el) {
        bucket_entry& buck = map_[find_bucket_idx(el->key)];
        lock(buck.version);
        internal_elem *prev = nullptr;
        internal_elem *curr = buck.head;
        while (curr != nullptr && curr != el) {
            prev = curr;
            curr = curr->next;
        }
        assert(curr);
        if (prev != nullptr)
            prev->next = curr->next;
        else
            buck.head = curr->next;
        unlock(buck.version);
        Transaction::rcu_delete(curr);
    }
    // non-transactional remove by key
    bool remove(const key_type& k) {
        bucket_entry& buck = map_[find_bucket_idx(k)];
        lock(buck.version);
        internal_elem *prev = nullptr;
        internal_elem *curr = buck.head;
        while (curr != nullptr && !pred(curr->key, k)) {
            prev = curr;
            curr = curr->next;
        }
        if (curr == nullptr) {
            unlock(buck.version);
            return false;
        }
        if (prev != nullptr)
            prev->next = curr->next;
        else
            buck.head = curr->next;
        unlock(buck.version);
        return true;
    }
    // insert a k-v node to a bucket
    void insert_in_bucket(bucket_entry& buck, const key_type& k, const value_type *v, bool valid) {
        assert(buck.version.is_locked());

        internal_elem *new_head = new internal_elem(k, v ? *v : value_type(), valid);
        internal_elem *curr_head = buck.head;

        new_head->next = curr_head;
        buck.head = new_head;

        buck.version.inc_nonopaque_version();
    }
    // find a key's k-v node (internal_elem) within a bucket
    internal_elem *find_in_bucket(const bucket_entry& buck, const key_type& k) {
        internal_elem *curr = buck.head;
        while (curr && !pred_(curr->key, k))
            curr = curr->next;
        return curr;
    }

    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_bit;
    }
    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_bit;
    }
    static bool is_phantom(internal_elem *e, const TransItem& item) {
        return (!e->valid() && !has_insert(item));
    }

    // TransItem keys
    static bool is_bucket(const TransItem& item) {
        return item.key<uintptr_t>() & bucket_bit;
    }
    static uintptr_t make_bucket_key(const bucket_entry& bucket) {
        return (reinterpret_cast<uintptr_t>(&bucket) | bucket_bit);
    }
    static bucket_entry *bucket_address(const TransItem& item) {
        uintptr_t bucket_key = item.key<uintptr_t>();
        return reinterpret_cast<bucket_entry*>(bucket_key & ~bucket_bit);
    }

    // new select_for_update methods (optionally) acquiring locks
    static bool select_for_update(TransProxy& item, TLockVersion& vers) {
        return item.acquire_write(vers);
    }
    static bool select_for_update(TransProxy& item, TVersion& vers) {
        TVersion v = vers;
        fence();
        if (!item.observe(v))
            return false;
        item.add_write();
        return true;
    }
    static bool select_for_update(TransProxy& item, TNonopaqueVersion& vers) {
        TNonopaqueVersion v = vers;
        fence();
        if (!item.observe(v))
            return false;
        item.add_write();
        return true;
    }
    static bool select_for_overwrite(TransProxy& item, TLockVersion& vers, const value_type *vptr) {
        return item.acquire_write(vptr, vers);
    }
    static bool select_for_overwrite(TransProxy& item, TVersion& vers, const value_type* vptr) {
        (void)vers;
        item.add_write(vptr);
        return true;
    }
    static bool select_for_overwrite(TransProxy& item, TNonopaqueVersion& vers, const value_type* vptr) {
        (void)vers;
        item.add_write(vptr);
        return true;
    }

    static void copy_row(internal_elem *table_row, const value_type *value) {
        if (value == nullptr)
            return;
        table_row->value = *value;
    }

    void lock(bucket_version_type& buck_vers) {
        buck_vers.lock();
    }
    void unlock(bucket_version_type& buck_vers) {
        buck_vers.unlock();
    }
};

template <typename K, typename V,
          bool Opacity = false, bool Adaptive = false, bool ReadMyWrite = false>
class ordered_index : public TObject {
public:
    typedef K key_type;
    typedef V value_type;

    typedef typename std::conditional<Opacity, TVersion, TNonopaqueVersion>::type occ_version_type;
    typedef typename std::conditional<Adaptive, TLockVersion, occ_version_type>::type version_type;

    static constexpr typename version_type::type invalid_bit = TransactionTid::user_bit;
    static constexpr TransItem::flags_type insert_bit = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit << 1;
    static constexpr uintptr_t internode_bit = 1;

    static constexpr bool index_read_my_write = ReadMyWrite;

    struct internal_elem {
        version_type version;
        key_type key;
        value_type value;
        bool deleted;

        internal_elem(const key_type& k, const value_type& v, bool valid)
            : version(valid ? Sto::initialized_tid() : Sto::initialized_tid() | invalid_bit),
              key(k), value(v), deleted(false) {}

        bool valid() const {
            return !(version & invalid_bit);
        }
    };

    struct table_params : public Masstree::nodeparams<15,15> {
        typedef internal_elem* value_type;
        typedef Masstree::value_print<value_type> value_print_type;
        typedef threadinfo threadinfo_type;
    };

    typedef Masstree::Str Str;
    typedef Masstree::basic_table<table_params> table_type;
    typedef Masstree::unlocked_tcursor<table_params> unlocked_cursor_type;
    typedef Masstree::tcursor<table_params> cursor_type;
    typedef Masstree::leaf<table_params> leaf_type;

    typedef typename table_type::node_type node_type;
    typedef typename unlocked_cursor_type::nodeversion_value_type nodeversion_value_type;

    typedef std::tuple<bool, bool, uintptr_t, const value_type*> sel_return_type;
    typedef std::tuple<bool, bool>                               ins_return_type;
    typedef std::tuple<bool, bool>                               del_return_type;

    ordered_index(size_t init_size) {
        ordered_index();
        (void)init_size;
    }

    ordered_index() {
        if (ti == nullptr)
            ti = threadinfo::make(threadinfo::TI_MAIN, -1);
        table_.initialize(*ti);
        key_gen_ = 0;
    }

    uint64_t gen_key() {
        return fetch_and_add(&key_gen_);
    }

    sel_return_type
    select_row(const key_type& key, bool for_update = false) {
        Str k = static_cast<Str>(key);
        unlocked_cursor_type lp(table_, k);
        bool found = lp.find_unlocked(*ti);
        internal_elem *e = lp.value();
        if (found) {
            return select_row(e, for_update);
        } else {
            if (!register_internode_version(lp.node(), lp.full_version_value()))
                goto abort;
            return sel_return_type(true, false, 0, nullptr);
        }

    abort:
        return sel_return_type(false, false, 0, nullptr);
    }

    sel_return_type
    select_row(uintptr_t rid, bool for_update = false) {
        internal_elem *e = reinterpret_cast<internal_elem *>(rid);
        TransProxy item = Sto::item(this, e);

        if (is_phantom(e, item))
            goto abort;

        if (index_read_my_write) {
            if (has_delete(item)) {
                return sel_return_type(true, false, 0, nullptr);
            }
            if (item.has_write()) {
                value_type *vptr;
                if (has_insert(item))
                    vptr = &e->value;
                else
                    vptr = item.template write_value<value_type *>();
                return sel_return_type(true, true, reinterpret_cast<uintptr_t>(e), vptr);
            }
        }

        if (for_update) {
            if (!select_for_update(item, e->version))
                goto abort;
        } else {
            if (!item.observe(e->version))
                goto abort;
        }

        return sel_return_type(true, true, reinterpret_cast<uintptr_t>(e), &e->value);

    abort:
        return sel_return_type(false, false, 0, nullptr);
    }

    void update_row(uintptr_t rid, value_type *new_row) {
        auto item = Sto::item(this, reinterpret_cast<internal_elem *>(rid));
        assert(item.has_write() && !has_insert(item));
        item.add_write(new_row);
    }

    // insert assumes common case where the row doesn't exist in the table
    // if a row already exists, then use select (FOR UPDATE) instead
    ins_return_type
    insert_row(const key_type& key, const value_type *vptr, bool overwrite = false) {
        cursor_type lp(table_, key);
        bool found = lp.find_insert(*ti);
        if (found) {
            internal_elem *e = lp.value();
            lp.finish(0, *ti);

            TransProxy item = Sto::item(this, e);

            if (is_phantom(e, item))
                goto abort;

            if (index_read_my_write) {
                if (has_delete(item)) {
                    item.clear_flags(delete_bit).clear_write().template add_write(vptr);
                    return ins_return_type(true, false);
                }
            }

            if (overwrite) {
                if (!select_for_overwrite(item, e->version, vptr))
                    goto abort;
                if (index_read_my_write) {
                    if (has_insert(item)) {
                        copy_row(e, vptr);
                    }
                }
            } else {
                if (!item.observe(e->version))
                    goto abort;
            }

        } else {
            internal_elem *e = new internal_elem(key, vptr ? *vptr : value_type(),
                                                 false /*valid*/);
            lp.value() = e;

            auto orig_node = lp.node();
            auto orig_nv = lp.previous_full_version_value();
            auto new_nv = lp.next_full_version_value(1);

            lp.finish(1, *ti);
            fence();

            TransProxy item = Sto::item(this, e);
            item.template add_write<value_type *>(vptr);
            item.add_flags(insert_bit);

            if (!update_nodeversion(orig_node, orig_nv, new_nv))
                goto abort;
        }

        return ins_return_type(true, found);

    abort:
        return ins_return_type(false, false);
    }

    del_return_type
    delete_row(const key_type& key) {
        unlocked_cursor_type lp(table_, key);
        bool found = lp.find_unlcoked(*ti);
        if (found) {
            internal_elem *e = lp.value();
            TransProxy item = Sto::item(this, e);

            if (is_phantom(e, item))
                goto abort;

            if (index_read_my_write) {
                if (has_delete(item))
                    return del_return_type(true, false);
                if (!e->valid && has_insert(item)) {
                    item.add_flags(delete_bit);
                    return del_return_type(true, true);
                }
            }

            // select_for_update will register an observation and set the write bit of
            // the TItem
            if (!select_for_update(item, e->version))
                goto abort;
            fence();
            if (e->deleted)
                goto abort;
            item.add_flags(delete_bit);
        } else {
            if (!register_internode_version(lp.node(), lp.full_version_value()))
                goto abort;
        }

        return del_return_type(true, found);

    abort:
        return del_return_type(false, false);
    }

    template <typename Callback, bool Reverse>
    bool range_scan(const key_type& begin, const key_type& end, Callback callback) {
        auto node_callback = [&] (leaf_type* node,
            typename unlocked_cursor_type::nodeversion_value_type version) {
            return register_internode_version(node, version);
        };

        auto value_callback = [&] (const key_type& key, internal_elem *e, bool& ret) {
            TransProxy item = Sto::item(this, e);

            if (index_read_my_write) {
                if (has_delete(item)) {
                    ret = true;
                    return true;
                }
                if (item.has_write()) {
                    if (has_insert(item))
                        ret = callback(key, e->value);
                    else
                        ret = callback(key, *(item.template write_value<value_type *>()));
                    return true;
                }
            }

            bool ok;
            if (Adaptive) {
                ok = item.observe(e->version, true/*force occ*/);
            } else {
                ok = item.observe(e->version);
            }
            if (!ok)
                return false;

            // skip invalid (inserted but yet committed) values, but do not abort
            if (!e->valid()) {
                ret = true;
                return true;
            }

            ret = callback(key, reinterpret_cast<uintptr_t>(e), e->value);
            return true;
        };

        range_scanner<decltype(node_callback), decltype(value_callback), Reverse> scanner(end, node_callback, value_callback);
        table_.scan(begin, true, scanner, *ti);
        return scanner.scan_succeeded_;
    }

    static __thread typename table_params::threadinfo_type *ti;

protected:
    template <typename NodeCallback, typename ValueCallback, bool Reverse>
    class range_scanner {
    public:
        range_scanner(const Str upper, NodeCallback ncb, ValueCallback vcb) :
            boundary_(upper), boundary_compar_(false), scan_succeeded_(true),
            node_callback_(ncb), value_callback_(vcb) {}

        template <typename ITER, typename KEY>
        void check(const ITER& iter, const KEY& key) {
            int min = std::min(boundary_.length(), key.prefix_length());
            int cmp = memcmp(boundary_.data(), key.full_string().data(), min);
            if (!Reverse) {
                if (cmp < 0 || (cmp == 0 && boundary_.length() <= key.prefix_length()))
                    boundary_compar_ = true;
                else if (cmp == 0) {
                    uint64_t last_ikey = iter.node()->ikey0_[iter.permutation()[iter.permutation().size() - 1]];
                    uint64_t slice = string_slice<uint64_t>::make_comparable(boundary_.data() + key.prefix_length(),
                        std::min(boundary_.length() - key.prefix_length(), 8));
                    boundary_compar_ = (slice <= last_ikey);
                }
            } else {
                if (cmp >= 0)
                    boundary_compar_ = true;
            }
        }

        template <typename ITER>
        void visit_leaf(const ITER& iter, const Masstree::key<uint64_t>& key, threadinfo&) {
            if (!node_callback_(iter.node(), iter.full_version_value())) {
                scan_succeeded_ = false;
            }
            if (this->boundary_) {
                check(iter, key);
            }
        }

        bool visit_value(const Masstree::key<uint64_t>& key, internal_elem *e, threadinfo&) {
            if (this->boundary_compar_) {
                if ((Reverse && (boundary_ >= key.full_string())) ||
                    (!Reverse && (boundary_ <= key.full_string())))
                    return false;
            }
            bool visited;
            if (!value_callback_(key.full_string(), e, visited)) {
                scan_succeeded_ = false;
                return false;
            } else {
                if (!visited)
                    scan_succeeded_ = false;
                return visited;
            }
        }

        Str boundary_;
        bool boundary_compar_;
        bool scan_succeeded_;

        NodeCallback node_callback_;
        ValueCallback value_callback_;
    };

private:
    table_type table_;
    uint64_t key_gen_;

    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_bit;
    }
    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_bit;
    }
    static bool is_phantom(internal_elem *e, const TransItem& item) {
        return (!e->valid() && !has_insert(item));
    }

    bool register_internode_version(node_type *node, nodeversion_value_type nodeversion) {
        TransProxy item = Sto::item(this, get_internode_key(node));
        if (Opacity)
            return item.add_read_opaque(nodeversion);
        else
            return item.add_read(nodeversion);
    }
    bool update_internode_version(node_type *node,
            nodeversion_value_type prev_nv, nodeversion_value_type new_nv) {
        TransProxy item = Sto::item(this, get_internode_key(node));
        if (item.has_read() &&
                (prev_nv == item.template read_value<nodeversion_value_type>())) {
            item.update_read(prev_nv, new_nv);
            return true;
        }
        return false;
    }

    static uintptr_t get_internode_key(node_type* node) {
        return reinterpret_cast<uintptr_t>(node) | internode_bit;
    }

    // new select_for_update methods (optionally) acquiring locks
    static bool select_for_update(TransProxy& item, TLockVersion& vers) {
        return item.acquire_write(vers);
    }
    static bool select_for_update(TransProxy& item, TVersion& vers) {
        TVersion v = vers;
        fence();
        if (!item.observe(v))
            return false;
        item.add_write();
        return true;
    }
    static bool select_for_update(TransProxy& item, TNonopaqueVersion& vers) {
        TNonopaqueVersion v = vers;
        fence();
        if (!item.observe(v))
            return false;
        item.add_write();
        return true;
    }
    static bool select_for_overwrite(TransProxy& item, TLockVersion& vers, const value_type *vptr) {
        return item.acquire_write(vptr, vers);
    }
    static bool select_for_overwrite(TransProxy& item, TVersion& vers, const value_type* vptr) {
        (void)vers;
        item.add_write(vptr);
        return true;
    }
    static bool select_for_overwrite(TransProxy& item, TNonopaqueVersion& vers, const value_type* vptr) {
        (void)vers;
        item.add_write(vptr);
        return true;
    }

    static void copy_row(internal_elem *e, const value_type *new_row) {
        if (new_row == nullptr)
            return;
        e->value = *new_row;
    }
};

template <typename K, typename V, bool Opacity, bool Adaptive, bool ReadMyWrite>
__thread typename ordered_index<K, V, Opacity, Adaptive, ReadMyWrite>::table_params::threadinfo_type
*ordered_index<K, V, Opacity, Adaptive, ReadMyWrite>::ti;

}; // namespace tpcc