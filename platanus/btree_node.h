// Copyright 2013 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PLATANUS_BTREE_NODE_H_
#define PLATANUS_BTREE_NODE_H_

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <type_traits>

#include "btree_util.h"

namespace platanus {

template <std::size_t BitWidth>
class btree_node_search_result {
 public:
  static_assert(BitWidth > 0 && BitWidth < 16, "BitWidth must be in the range [1, 15].");

  static constexpr std::size_t bit_width         = BitWidth;
  static constexpr bool        is_less_than_8bit = bit_width < 8;

  using count_type = std::conditional_t<is_less_than_8bit, std::uint_least8_t, std::uint_least16_t>;
  using signed_count_type =
      std::conditional_t<is_less_than_8bit, std::int_least16_t, std::int_least32_t>;

  btree_node_search_result()                                           = default;
  btree_node_search_result(const btree_node_search_result&)            = default;
  btree_node_search_result& operator=(const btree_node_search_result&) = default;
  btree_node_search_result(btree_node_search_result&&)                 = default;
  btree_node_search_result& operator=(btree_node_search_result&&)      = default;
  ~btree_node_search_result()                                          = default;

  explicit btree_node_search_result(count_type index, bool is_exact_match) noexcept
      : index_(index), exact_match_(is_exact_match ? 1 : 0) {
    assert(index < (1 << bit_width));
  }

  count_type index() const noexcept { return index_; }
  bool       is_exact_match() const noexcept { return exact_match_ == 1; }

 private:
  count_type index_ : bit_width{0};
  count_type exact_match_ : 1 {0};
};

// A node in the btree holding. The same node type is used for both internal
// and leaf nodes in the btree, though the nodes are allocated in such a way
// that the children array is only valid in internal nodes.
template <typename Params>
class btree_node {
 public:
  using params_type = Params;
  using self_type   = btree_node<Params>;
  using key_type    = typename Params::key_type;
  // Deprecated: use mapped_type instead.
  using data_type          = typename Params::data_type;
  using mapped_type        = typename Params::mapped_type;
  using value_type         = typename Params::value_type;
  using mutable_value_type = typename Params::mutable_value_type;
  using pointer            = typename Params::pointer;
  using const_pointer      = typename Params::const_pointer;
  using reference          = typename Params::reference;
  using const_reference    = typename Params::const_reference;
  using key_compare        = typename Params::key_compare;
  using value_compare      = typename Params::value_compare;
  using size_type          = typename Params::size_type;
  using difference_type    = typename Params::difference_type;

  static constexpr std::size_t kValueSize      = params_type::kValueSize;
  static constexpr std::size_t kTargetNodeSize = params_type::kTargetNodeSize;

  // Available space for values.
  static_assert(
      kTargetNodeSize >= 3 * sizeof(void*), "ValueSize must be no less than 3 * sizeof(void*)"
  );
  static constexpr std::size_t kNodeValueSpace = kTargetNodeSize - 3 * sizeof(void*);

  static constexpr std::size_t kNodeTargetValues = kNodeValueSpace / kValueSize;
  // We need a minimum of 3 values per internal node in order to perform
  // splitting (1 value for the two nodes involved in the split and 1 value
  // propagated to the parent as the delimiter for the split).
  static constexpr std::size_t kNodeValues   = kNodeTargetValues >= 3 ? kNodeTargetValues : 3;
  static constexpr std::size_t kNodeChildren = kNodeValues + 1;

  using search_result =
      btree_node_search_result<std::bit_width(static_cast<std::size_t>(kNodeChildren - 1))>;
  using count_type        = typename search_result::count_type;
  using signed_count_type = typename search_result::signed_count_type;

  using values_type                   = std::array<mutable_value_type, kNodeValues>;
  using values_iterator               = typename values_type::iterator;
  using values_const_iterator         = typename values_type::const_iterator;
  using values_reverse_iterator       = typename values_type::reverse_iterator;
  using values_const_reverse_iterator = typename values_type::const_reverse_iterator;

  using allocator_type   = typename Params::allocator_type;
  using allocator_traits = std::allocator_traits<allocator_type>;

  using node_allocator_type   = typename allocator_traits::template rebind_alloc<btree_node>;
  using node_allocator_traits = std::allocator_traits<node_allocator_type>;

  class node_deleter {
   public:
    using pointer = typename node_allocator_traits::pointer;

    node_deleter()                               = default;
    node_deleter(const node_deleter&)            = default;
    node_deleter(node_deleter&&)                 = default;
    node_deleter& operator=(const node_deleter&) = default;
    node_deleter& operator=(node_deleter&&)      = default;
    ~node_deleter()                              = default;

    explicit node_deleter(node_allocator_type& alloc) : alloc_(&alloc) {}

    void operator()(pointer p) {
      if (p == nullptr) {
        return;
      }
      assert(alloc_ != nullptr);
      node_allocator_traits::destroy(*alloc_, p);
      node_allocator_traits::deallocate(*alloc_, p, 1);
    }

    void swap(node_deleter& x) noexcept { btree_swap_helper(alloc_, x.alloc_); }

   private:
    node_allocator_type* alloc_;
  };

  // node_owner owes to allocate and release the memory of the node.
  using node_owner = std::unique_ptr<btree_node, node_deleter>;
  // node_borrower can change the node's content but not the memory (allocation and release not
  // allowed).
  using node_borrower = btree_node*;

  using children_allocator_type   = typename allocator_traits::template rebind_alloc<node_owner>;
  using children_allocator_traits = std::allocator_traits<children_allocator_type>;

  class children_deleter {
   public:
    using pointer = typename children_allocator_traits::pointer;

    children_deleter()                                   = default;
    children_deleter(const children_deleter&)            = default;
    children_deleter(children_deleter&&)                 = default;
    children_deleter& operator=(const children_deleter&) = default;
    children_deleter& operator=(children_deleter&&)      = default;
    ~children_deleter()                                  = default;

    explicit children_deleter(children_allocator_type& alloc) : alloc_(&alloc) {}

    void operator()(pointer p) {
      if (p == nullptr) {
        return;
      }
      assert(alloc_ != nullptr);
      for (count_type i = 0; i < kValueSize + 1; ++i) {
        if (not p[i]) {
          children_allocator_traits::destroy(*alloc_, &p[i]);
        }
      }
      children_allocator_traits::deallocate(*alloc_, p, kNodeChildren);
    }

    void swap(children_deleter& x) noexcept { btree_swap_helper(alloc_, x.alloc_); }

   private:
    children_allocator_type* alloc_;
  };

  using children_ptr                    = std::unique_ptr<node_owner[], children_deleter>;
  using children_iterator               = typename children_ptr::pointer;
  using children_const_iterator         = const children_iterator;
  using children_reverse_iterator       = std::reverse_iterator<children_iterator>;
  using children_const_reverse_iterator = std::reverse_iterator<children_const_iterator>;

  static node_owner make_node(
      bool                     is_leaf,
      node_borrower            parent,
      node_allocator_type&     node_alloc,
      children_allocator_type& children_alloc
  ) {
    auto node_ptr = node_allocator_traits::allocate(node_alloc, 1);
    node_allocator_traits::construct(node_alloc, node_ptr, is_leaf, parent, children_alloc);
    return node_owner(node_ptr, node_deleter{node_alloc});
  }

  static node_owner make_leaf_root_node(
      node_allocator_type& node_alloc, children_allocator_type& children_alloc
  ) {
    auto node_ptr = node_allocator_traits::allocate(node_alloc, 1);
    node_allocator_traits::construct(node_alloc, node_ptr, true, node_ptr, children_alloc);
    return node_owner(node_ptr, node_deleter{node_alloc});
  }

 public:
  btree_node()                        = default;
  btree_node(btree_node&&)            = default;
  btree_node& operator=(btree_node&&) = default;
  ~btree_node()                       = default;

  btree_node(const btree_node&)     = delete;
  void operator=(const btree_node&) = delete;

  explicit btree_node(bool is_leaf, node_borrower parent, children_allocator_type& children_alloc)
      : children_ptr_(), parent_(parent), position_(0), count_(0) {
    if (not is_leaf) {
      auto p = children_allocator_traits::allocate(children_alloc, kNodeChildren);
      for (count_type i = 0; i < kNodeChildren; ++i) {
        children_allocator_traits::construct(children_alloc, &p[i], nullptr);
      }
      children_ptr_ = children_ptr(p, children_deleter{children_alloc});
    }
  }

  // Get this node borrower.
  node_borrower     borrow_myself() noexcept { return this; }
  const btree_node* borrow_readonly_myself() const noexcept {
    return static_cast<const btree_node*>(this);
  }

  // Getter/setter for whether this is a leaf node or not. This value doesn't
  // change after the node is created.
  bool leaf() const noexcept { return children_ptr_ ? false : true; }

  // Getter for the position of this node in its parent.
  count_type position() const noexcept { return position_; }
  void       set_position(count_type v) noexcept {
          assert(borrow_parent() != nullptr);
          assert(0 <= v && v < borrow_parent()->max_children_count());
          position_ = v;
  }

  // Getter/setter for the number of values stored in this node.
  count_type           count() const noexcept { return count_; }
  constexpr count_type max_count() const noexcept { return kNodeValues; }

  // Getter for the parent of this node.
  node_borrower       borrow_parent() const noexcept { return parent_; }
  const node_borrower borrow_readonly_parent() const noexcept { return parent_; }
  // Getter for whether the node is the root of the tree. The parent of the
  // root of the tree is the leftmost node in the tree which is guaranteed to
  // be a leaf.
  bool is_root() const noexcept { return borrow_readonly_parent()->leaf(); }
  void make_root() noexcept {
    assert(borrow_readonly_parent()->is_root());
    parent_ = parent_->borrow_parent();
  }

  // Getters for the key/value at position i in the node.
  const key_type& key(count_type i) const noexcept { return params_type::key(values_[i]); }
  reference       value(count_type i) noexcept { return reinterpret_cast<reference>(values_[i]); }
  const_reference value(count_type i) const noexcept {
    return reinterpret_cast<const_reference>(values_[i]);
  }

  // Swap value i in this node with value j in node x.
  void value_swap(count_type i, node_borrower x, count_type j) noexcept(noexcept(
      params_type::swap(std::declval<mutable_value_type&>(), std::declval<mutable_value_type&>())
  )) {
    params_type::swap(values_[i], x->values_[j]);
  }

  // Getters/setter for the child at position i in the node.
  node_borrower       borrow_child(count_type i) const noexcept { return children_ptr_[i].get(); }
  const node_borrower borrow_readonly_child(count_type i) const noexcept {
    return children_ptr_[i].get();
  }
  node_owner extract_child(count_type i) noexcept { return std::move(children_ptr_[i]); }
  void       set_child(count_type i, node_owner&& new_child) noexcept {
          children_ptr_[i]              = std::move(new_child);
          auto borrowed_new_child       = borrow_child(i);
          borrowed_new_child->parent_   = borrow_myself();
          borrowed_new_child->position_ = i;
  }

  // Returns the position of the first value whose key is not less than k.
  search_result lower_bound(const key_type& k, const key_compare& comp) const
      noexcept(noexcept(binary_search_compare(k, 0, count(), comp))) {
    return binary_search_compare(k, 0, count(), comp);
  }
  // Returns the position of the first value whose key is greater than k.
  search_result upper_bound(const key_type& k, const key_compare& comp) const
      noexcept(noexcept(binary_search_compare<false>(k, 0, count(), comp))) {
    return binary_search_compare<false>(k, 0, count(), comp);
  }

  // Returns the position of the first value whose key is not less than k using
  // binary search performed.
  template <bool WithEqual = true>
  search_result binary_search_compare(
      const key_type& k, count_type s, count_type e, const key_compare& comp
  ) const
      noexcept(noexcept(comp(std::declval<const key_type&>(), k))) {
    while (s != e) {
      count_type mid = (s + e) / 2;
      auto        c   = comp(key(mid), k);
      if (c < 0) {
        s = mid + 1;
      } else if (c > 0) {
        e = mid;
      } else {
        if constexpr (WithEqual) {
          // Need to return the first value whose key is not less than k, which
          // requires continuing the binary search. Note that we are guaranteed
          // that the result is an exact match because if "key(mid-1) < k" the
          // call to binary_search_compare() will return "mid".
          s = binary_search_compare(k, s, mid, comp).index();
          return search_result(s, true);
        } else {
          s = mid + 1;
        }
      }
    }
    return search_result(s, false);
  }

  // Returns the pointer to the front of the values array.
  values_iterator begin_values() noexcept { return values_.begin(); }

  // Returns the pointer to the back of the values array.
  values_iterator end_values(
  ) noexcept(noexcept(std::next(std::declval<values_iterator>(), std::declval<count_type>()))) {
    return std::next(begin_values(), count());
  }

  values_reverse_iterator rbegin_values() noexcept(noexcept(std::reverse_iterator(end_values()))) {
    return std::reverse_iterator(end_values());
  }
  values_reverse_iterator rend_values() noexcept { return std::reverse_iterator(begin_values()); }

  // Returns the pointer to the front of the children array.
  children_iterator begin_children() noexcept { return children_ptr_.get(); }

  // Returns the pointer to the back of the children array.
  children_iterator end_children(
  ) noexcept(noexcept(std::next(std::declval<children_iterator>(), std::declval<int>()))) {
    return std::next(begin_children(), count() + 1);
  }

  children_reverse_iterator rbegin_children() noexcept(noexcept(std::reverse_iterator(end_children()
  ))) {
    return std::reverse_iterator(end_children());
  }
  children_reverse_iterator rend_children() noexcept(noexcept(std::reverse_iterator(begin_children()
  ))) {
    return std::reverse_iterator(begin_children());
  }

  count_type values_count() const noexcept { return count(); }
  count_type children_count() const noexcept { return leaf() ? 0 : count() + 1; }

  count_type max_values_count() const noexcept { return max_count(); }
  count_type max_children_count() const noexcept { return leaf() ? 0 : max_count() + 1; }

  // Rotate the values in the range [first, last) to the left.
  // As a result, the value pointed by middle will now be the first value and
  // the value pointed by middle - 1 will be the last value.
  // Example: let values be [0, 1, 2, 3, 4, 5], first = 1, middle = 3, last = 6,
  // after rotate_value_left(1, 3, 6) the values will be [0, 3, 4, 5, 1, 2].
  void rotate_values(int first, int middle, int last) {
    assert(0 <= first && first <= middle && middle <= last && last <= max_values_count());
    auto vbegin = begin_values();
    std::rotate(vbegin + first, vbegin + middle, vbegin + last);
  }

  // Shift the children in the range [first, last) to the right by shift.
  // CAUTION: This function  uninitializes [first, first + shift) in *this.
  void shift_children_right(int first, int last, int shift) {
    assert(0 <= first && first <= last && 0 <= shift && last + shift <= max_children_count());

    auto begin = begin_children() + first;
    auto end   = begin_children() + last;

    std::rotate(begin, end, end + shift);
    std::for_each(begin + shift, end + shift, [shift](node_owner& np) {
      np->set_position(np->position() + shift);
    });
    std::fill_n(begin, shift, nullptr);
  }

  // Shift the children in the range [first, last) to the left by shift.
  // CAUTION: This function  uninitializes [last - shift, last) in *this.
  void shift_children_left(int first, int last, int shift) {
    assert(0 <= first && first <= last && last <= max_children_count());
    assert(0 <= shift && 0 <= first - shift);

    auto begin = begin_children() + first;
    auto end   = begin_children() + last;

    std::rotate(begin - shift, begin, end);
    std::for_each(begin - shift, end - shift, [shift](node_owner& np) {
      np->set_position(np->position() - shift);
    });
    std::fill_n(end - shift, shift, nullptr);
  }

  // Receive the children from [first, last) in *src and set them to [dest, dest + last - first) in
  // *this. CAUTION: This function doesn't uninitialize [first, last) in *src.
  void receive_children(
      children_iterator dest, node_borrower src, children_iterator first, children_iterator last
  ) {
    auto n = last - first;
    assert(0 <= n && n <= std::numeric_limits<int>::max());
    receive_children_n(dest, src, first, static_cast<int>(n));
  }

  // Receive the children from [first, first + n) in *src and set them to [dest, dest + n) in *this.
  // CAUTION: This function doesn't uninitialize [first, last) in *src.
  void receive_children_n(
      children_iterator dest, node_borrower src, children_iterator first, int n
  ) {
    auto dest_idx  = dest - begin_children();
    auto first_idx = first - src->begin_children();
    assert(0 <= dest_idx && 0 <= first_idx);
    assert(
        0 <= n && dest_idx + n <= max_children_count() && first_idx + n <= src->max_children_count()
    );
    for (int i = 0; i < n; ++i) {
      set_child(dest_idx + i, src->extract_child(first_idx + i));
    }
  }

  // Rotate the values [values.begin() + i, values.end()) so that the last value is at position i.
  void rotate_back_to(int i);

  // Inserts the value x at position i, shifting all existing values and
  // children at positions >= i to the right by 1.
  void insert_value(int i, const value_type& x);

  // Emplaces the value at position i, shifting all existing values and children
  // at positions >= i to the right by 1.
  template <typename... Args>
  void emplace_value(int i, Args&&... args);

  // Removes the value at position i, shifting all existing values and children
  // at positions > i to the left by 1.
  void remove_value(int i);

  // Rebalances a node with its right sibling.
  void rebalance_right_to_left(node_borrower sibling, int to_move);
  void rebalance_left_to_right(node_borrower sibling, int to_move);

  // Splits a node, moving a portion of the node's values to its right sibling.
  void split(node_owner&& sibling, int insert_position);

  // Merges a node with its right sibling, moving all of the values and the
  // delimiting key in the parent node onto itself.
  void merge(node_borrower sibling);

  // Swap the contents of "this" and "src".
  void swap(btree_node& src);

 private:
  void set_count(count_type v) noexcept {
    assert(0 <= v && v <= max_count());
    count_ = v;
  }

  template <typename... Args>
  void value_init(int i, Args&&... args) {
    values_[i] = mutable_value_type{std::forward<Args>(args)...};
  }
  void value_init(int i, const value_type& x) { values_[i] = x; }

 private:
  // The pointer to the array of values.
  values_type values_;
  // The pointer to the array of child pointers. The keys in children_[i] are all less than
  // key(i). The keys in children_[i + 1] are all greater than key(i). There
  // are always count + 1 children.
  children_ptr children_ptr_;
  // A pointer to the node's parent.
  node_borrower parent_;
  // The position of the node in the node's parent.
  count_type position_;
  // The count of the number of values in the node.
  count_type count_;
};

////
// btree_node methods
template <typename P>
inline void btree_node<P>::rotate_back_to(int i) {
  assert(i <= count());
  rotate_values(i, values_count(), values_count() + 1);
  set_count(count() + 1);

  if (!leaf()) {
    ++i;
    shift_children_right(i, children_count() - 1, 1);
  }
}

template <typename P>
inline void btree_node<P>::insert_value(int i, const value_type& x) {
  value_init(values_count(), x);
  rotate_back_to(i);
}

template <typename P>
template <typename... Args>
inline void btree_node<P>::emplace_value(int i, Args&&... args) {
  value_init(values_count(), std::forward<Args>(args)...);
  rotate_back_to(i);
}

template <typename P>
inline void btree_node<P>::remove_value(int i) {
  if (!leaf()) {
    assert(borrow_child(i + 1)->count() == 0);
    if (i + 2 < children_count()) {
      shift_children_left(i + 2, children_count(), 1);
    }
  }

  auto old_values_count = values_count();
  set_count(count() - 1);
  if (i + 1 < old_values_count) {
    rotate_values(i, i + 1, old_values_count);
  }
}

template <typename P>
void btree_node<P>::rebalance_right_to_left(node_borrower src, int to_move) {
  assert(borrow_readonly_parent() == src->borrow_readonly_parent());
  assert(borrow_readonly_parent() != nullptr);
  assert(position() + 1 == src->position());
  assert(src->count() >= count());
  assert(to_move >= 1);
  assert(to_move <= src->count());

  // Move the delimiting value to the left node and the new delimiting value
  // from the right node.
  value_swap(values_count(), borrow_parent(), position());
  borrow_parent()->value_swap(position(), src, to_move - 1);

  // Move the values from the right to the left node.
  std::swap_ranges(end_values() + 1, end_values() + to_move, src->begin_values());
  // Shift the values in the right node to their correct position.
  src->rotate_values(0, to_move, src->count());

  if (!leaf()) {
    // Move the child pointers from the right to the left node.
    receive_children_n(end_children(), src, src->begin_children(), to_move);
    src->shift_children_left(to_move, src->children_count(), to_move);
  }

  // Fixup the counts on the src and dest nodes.
  set_count(count() + to_move);
  src->set_count(src->count() - to_move);
}

template <typename P>
void btree_node<P>::rebalance_left_to_right(node_borrower dest, int to_move) {
  assert(borrow_readonly_parent() == dest->borrow_readonly_parent());
  assert(borrow_readonly_parent() != nullptr);
  assert(position() + 1 == dest->position());
  assert(count() >= dest->count());
  assert(to_move >= 1);
  assert(to_move <= count());

  dest->rotate_values(0, dest->values_count(), dest->values_count() + to_move);

  // Move the delimiting value to the right node and the new delimiting value
  // from the left node.
  dest->value_swap(to_move - 1, borrow_parent(), position());
  borrow_parent()->value_swap(position(), this, values_count() - to_move);

  // Move the values from the left to the right node.
  std::swap_ranges(end_values() - to_move + 1, end_values(), dest->begin_values());

  if (!leaf()) {
    // Move the child pointers from the left to the right node.
    dest->shift_children_right(0, dest->children_count(), to_move);
    dest->receive_children_n(
        dest->begin_children(),
        this,
        begin_children() + children_count() - to_move,
        to_move
    );
    std::fill_n(rbegin_children(), to_move, nullptr);
  }

  // Fixup the counts on the src and dest nodes.
  set_count(count() - to_move);
  dest->set_count(dest->count() + to_move);
}

template <typename P>
void btree_node<P>::split(node_owner&& dest, int insert_position) {
  assert(dest->count() == 0);
  assert(borrow_readonly_parent() != nullptr);

  // We bias the split based on the position being inserted. If we're
  // inserting at the beginning of the left node then bias the split to put
  // more values on the right node. If we're inserting at the end of the
  // right node then bias the split to put more values on the left node.
  if (insert_position == 0) {
    dest->set_count(count() - 1);
  } else if (insert_position == max_children_count() - 1) {
    dest->set_count(0);
  } else {
    dest->set_count(count() / 2);
  }
  set_count(count() - dest->count());
  assert(count() >= 1);

  std::move(end_values(), end_values() + dest->count(), dest->begin_values());

  // The split key is the largest value in the left sibling.
  set_count(count() - 1);
  borrow_parent()->emplace_value(position());
  value_swap(values_count(), borrow_parent(), position());
  borrow_parent()->set_child(position() + 1, std::move(dest));

  if (!leaf()) {
    auto dest = borrow_parent()->borrow_child(position() + 1);
    for (int i = 0; i <= dest->count(); ++i) {
      assert(borrow_child(children_count() + i) != nullptr);
    }
    dest->receive_children_n(dest->begin_children(), this, end_children(), dest->count() + 1);
    std::fill_n(end_children(), dest->count() + 1, nullptr);
  }
}

template <typename P>
void btree_node<P>::merge(node_borrower src) {
  assert(borrow_readonly_parent() == src->borrow_readonly_parent());
  assert(borrow_readonly_parent() != nullptr);
  assert(position() + 1 == src->position());

  // Move the delimiting value to the left node.
  value_init(values_count());
  value_swap(values_count(), borrow_parent(), position());

  // Move the values from the right to the left node.
  std::swap_ranges(src->begin_values(), src->end_values(), end_values() + 1);

  if (!leaf()) {
    // Move the child pointers from the right to the left node.
    receive_children(end_children(), src, src->begin_children(), src->end_children());
  }

  // Fixup the counts on the src and dest nodes.
  set_count(1 + count() + src->count());
  src->set_count(0);

  // Remove the value on the parent node.
  borrow_parent()->remove_value(position());
}

template <typename P>
void btree_node<P>::swap(btree_node& x) {
  // Swap the values.
  btree_swap_helper(values_, x.values_);

  if (!leaf() || !x.leaf()) {
    // Swap the child pointers.
    btree_swap_helper(children_ptr_, x.children_ptr_);
    std::for_each_n(x.begin_children(), children_count(), [x](node_borrower np) {
      np->parent_ = x.borrow_myself();
    });
    std::for_each_n(begin_children(), x.children_count(), [this](node_borrower np) {
      np->parent_ = borrow_myself();
    });
  }
  btree_swap_helper(parent_, x.parent_);
  btree_swap_helper(position_, x.position_);
  btree_swap_helper(count_, x.count_);
}

}  // namespace platanus

#endif  // PLATANUS_BTREE_NODE_H_