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

#ifndef CPPBTREE_BTREE_COMPARER_H_
#define CPPBTREE_BTREE_COMPARER_H_

#include <functional>
#include <string>
#include <type_traits>

namespace cppbtree {

// A helper type used to indicate that a key-compare-to functor has been
// provided. A user can specify a key-compare-to functor by doing:
//
//  struct MyStringComparer
//      : public util::btree::btree_key_compare_to_tag {
//    int operator()(const string &a, const string &b) const {
//      return a.compare(b);
//    }
//  };
//
// Note that the return type is an int and not a bool. There is a
// COMPILE_ASSERT which enforces this return type.
struct btree_key_compare_to_tag {};

// A helper class that indicates if the Compare parameter is derived from
// btree_key_compare_to_tag.
template <typename Compare>
struct btree_is_key_compare_to : public std::is_convertible<Compare, btree_key_compare_to_tag> {};

// A helper class to convert a boolean comparison into a three-way
// "compare-to" comparison that returns a negative value to indicate
// less-than, zero to indicate equality and a positive value to
// indicate greater-than. This helper class is specialized for
// less<string> and greater<string>. The btree_key_compare_to_adapter
// class is provided so that btree users automatically get the more
// efficient compare-to code when using common google string types
// with common comparison functors.
template <typename Compare>
struct btree_key_compare_to_adapter : Compare {
  btree_key_compare_to_adapter()                                               = default;
  btree_key_compare_to_adapter(const btree_key_compare_to_adapter&)            = default;
  btree_key_compare_to_adapter(btree_key_compare_to_adapter&&)                 = default;
  btree_key_compare_to_adapter& operator=(const btree_key_compare_to_adapter&) = default;
  btree_key_compare_to_adapter& operator=(btree_key_compare_to_adapter&&)      = default;
  ~btree_key_compare_to_adapter()                                              = default;

  btree_key_compare_to_adapter(const Compare& c) : Compare(c) {}
};

template <>
struct btree_key_compare_to_adapter<std::less<std::string>> : public btree_key_compare_to_tag {
  btree_key_compare_to_adapter()                                               = default;
  btree_key_compare_to_adapter(const btree_key_compare_to_adapter&)            = default;
  btree_key_compare_to_adapter(btree_key_compare_to_adapter&&)                 = default;
  btree_key_compare_to_adapter& operator=(const btree_key_compare_to_adapter&) = default;
  btree_key_compare_to_adapter& operator=(btree_key_compare_to_adapter&&)      = default;
  ~btree_key_compare_to_adapter()                                              = default;

  btree_key_compare_to_adapter(const std::less<std::string>&) {}

  int operator()(const std::string& a, const std::string& b) const { return a.compare(b); }
};

template <>
struct btree_key_compare_to_adapter<std::greater<std::string>> : public btree_key_compare_to_tag {
  btree_key_compare_to_adapter()                                               = default;
  btree_key_compare_to_adapter(const btree_key_compare_to_adapter&)            = default;
  btree_key_compare_to_adapter(btree_key_compare_to_adapter&&)                 = default;
  btree_key_compare_to_adapter& operator=(const btree_key_compare_to_adapter&) = default;
  btree_key_compare_to_adapter& operator=(btree_key_compare_to_adapter&&)      = default;
  ~btree_key_compare_to_adapter()                                              = default;

  btree_key_compare_to_adapter(const std::greater<std::string>&) {}

  int operator()(const std::string& a, const std::string& b) const { return b.compare(a); }
};

// A helper class that allows a compare-to functor to behave like a plain
// compare functor. This specialization is used when we do not have a
// compare-to functor.
template <typename Key, typename Compare, bool HaveCompareTo>
struct btree_key_comparer {
  btree_key_comparer()                                     = default;
  btree_key_comparer(const btree_key_comparer&)            = default;
  btree_key_comparer(btree_key_comparer&&)                 = default;
  btree_key_comparer& operator=(const btree_key_comparer&) = default;
  btree_key_comparer& operator=(btree_key_comparer&&)      = default;
  ~btree_key_comparer()                                    = default;

  btree_key_comparer(Compare c) : comp(c) {}
  static bool bool_compare(const Compare& comp, const Key& x, const Key& y) { return comp(x, y); }
  bool        operator()(const Key& x, const Key& y) const { return bool_compare(comp, x, y); }
  Compare     comp;
};

// A specialization of btree_key_comparer when a compare-to functor is
// present. We need a plain (boolean) comparison in some parts of the btree
// code, such as insert-with-hint.
template <typename Key, typename Compare>
struct btree_key_comparer<Key, Compare, true> {
  btree_key_comparer()                                     = default;
  btree_key_comparer(const btree_key_comparer&)            = default;
  btree_key_comparer(btree_key_comparer&&)                 = default;
  btree_key_comparer& operator=(const btree_key_comparer&) = default;
  btree_key_comparer& operator=(btree_key_comparer&&)      = default;
  ~btree_key_comparer()                                    = default;

  btree_key_comparer(Compare c) : comp(c) {}
  static bool bool_compare(const Compare& comp, const Key& x, const Key& y) {
    return comp(x, y) < 0;
  }
  bool    operator()(const Key& x, const Key& y) const { return bool_compare(comp, x, y); }
  Compare comp;
};

// A helper function to compare to keys using the specified compare
// functor. This dispatches to the appropriate btree_key_comparer comparison,
// depending on whether we have a compare-to functor or not (which depends on
// whether Compare is derived from btree_key_compare_to_tag).
template <typename Key, typename Compare>
static bool btree_compare_keys(const Compare& comp, const Key& x, const Key& y) {
  using key_comparer = btree_key_comparer<Key, Compare, btree_is_key_compare_to<Compare>::value>;
  return key_comparer::bool_compare(comp, x, y);
}

// An adapter class that converts a lower-bound compare into an upper-bound
// compare.
template <typename Key, typename Compare>
struct btree_upper_bound_adapter : public Compare {
  btree_upper_bound_adapter()                                            = default;
  btree_upper_bound_adapter(const btree_upper_bound_adapter&)            = default;
  btree_upper_bound_adapter(btree_upper_bound_adapter&&)                 = default;
  btree_upper_bound_adapter& operator=(const btree_upper_bound_adapter&) = default;
  btree_upper_bound_adapter& operator=(btree_upper_bound_adapter&&)      = default;
  ~btree_upper_bound_adapter()                                           = default;

  btree_upper_bound_adapter(Compare c) : Compare(c) {}
  bool operator()(const Key& a, const Key& b) const {
    return !static_cast<const Compare&>(*this)(b, a);
  }
};

template <typename Key, typename CompareTo>
struct btree_upper_bound_compare_to_adapter : public CompareTo {
  btree_upper_bound_compare_to_adapter()                                            = default;
  btree_upper_bound_compare_to_adapter(const btree_upper_bound_compare_to_adapter&) = default;
  btree_upper_bound_compare_to_adapter(btree_upper_bound_compare_to_adapter&&)      = default;
  btree_upper_bound_compare_to_adapter& operator=(const btree_upper_bound_compare_to_adapter&) =
      default;
  btree_upper_bound_compare_to_adapter& operator=(btree_upper_bound_compare_to_adapter&&) = default;
  ~btree_upper_bound_compare_to_adapter()                                                 = default;

  btree_upper_bound_compare_to_adapter(CompareTo c) : CompareTo(c) {}
  int operator()(const Key& a, const Key& b) const {
    return static_cast<const CompareTo&>(*this)(b, a);
  }
};

// Dispatch helper class for using linear search with plain compare.
template <typename K, typename N, typename Compare>
struct btree_linear_search_plain_compare {
  static int lower_bound(const K& k, const N& n, Compare comp) {
    return n.linear_search_plain_compare(k, 0, n.count(), comp);
  }
  static int upper_bound(const K& k, const N& n, Compare comp) {
    using upper_compare = btree_upper_bound_adapter<K, Compare>;
    return n.linear_search_plain_compare(k, 0, n.count(), upper_compare(comp));
  }
};

// Dispatch helper class for using linear search with compare-to
template <typename K, typename N, typename CompareTo>
struct btree_linear_search_compare_to {
  static int lower_bound(const K& k, const N& n, CompareTo comp) {
    return n.linear_search_compare_to(k, 0, n.count(), comp);
  }
  static int upper_bound(const K& k, const N& n, CompareTo comp) {
    using upper_compare = btree_upper_bound_adapter<K, btree_key_comparer<K, CompareTo, true>>;
    return n.linear_search_plain_compare(k, 0, n.count(), upper_compare(comp));
  }
};

// Dispatch helper class for using binary search with plain compare.
template <typename K, typename N, typename Compare>
struct btree_binary_search_plain_compare {
  static int lower_bound(const K& k, const N& n, Compare comp) {
    return n.binary_search_plain_compare(k, 0, n.count(), comp);
  }
  static int upper_bound(const K& k, const N& n, Compare comp) {
    using upper_compare = btree_upper_bound_adapter<K, Compare>;
    return n.binary_search_plain_compare(k, 0, n.count(), upper_compare(comp));
  }
};

// Dispatch helper class for using binary search with compare-to.
template <typename K, typename N, typename CompareTo>
struct btree_binary_search_compare_to {
  static int lower_bound(const K& k, const N& n, CompareTo comp) {
    return n.binary_search_compare_to(k, 0, n.count(), CompareTo());
  }
  static int upper_bound(const K& k, const N& n, CompareTo comp) {
    using upper_compare = btree_upper_bound_adapter<K, btree_key_comparer<K, CompareTo, true>>;
    return n.linear_search_plain_compare(k, 0, n.count(), upper_compare(comp));
  }
};

// Dispatch helper class for using btree::internal_locate with plain compare.
struct btree_internal_locate_plain_compare {
  template <typename K, typename T, typename Iter>
  static std::pair<Iter, int> dispatch(const K& k, const T& t, Iter iter) {
    return t.internal_locate_plain_compare(k, iter);
  }
};

// Dispatch helper class for using btree::internal_locate with compare-to.
struct btree_internal_locate_compare_to {
  template <typename K, typename T, typename Iter>
  static std::pair<Iter, int> dispatch(const K& k, const T& t, Iter iter) {
    return t.internal_locate_compare_to(k, iter);
  }
};

}  // namespace cppbtree

#endif  // CPPBTREE_BTREE_COMPARER_H_