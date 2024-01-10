#pragma once
#include "rpmostree-clientlib.h"
#include "rpmostree-container.hpp"
#include "rpmostree-cxxrsutil.hpp"
#include "rpmostree-diff.hpp"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-output.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include "rpmostreemain.h"
#include "src/libpriv/rpmostree-cxxrs-prelude.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace rust
{
inline namespace cxxbridge1
{
// #include "rust/cxx.h"

#ifndef CXXBRIDGE1_PANIC
#define CXXBRIDGE1_PANIC
template <typename Exception> void panic [[noreturn]] (const char *msg);
#endif // CXXBRIDGE1_PANIC

struct unsafe_bitcopy_t;

namespace
{
template <typename T> class impl;
} // namespace

template <typename T>::std::size_t size_of ();
template <typename T>::std::size_t align_of ();

#ifndef CXXBRIDGE1_RUST_STRING
#define CXXBRIDGE1_RUST_STRING
class String final
{
public:
  String () noexcept;
  String (const String &) noexcept;
  String (String &&) noexcept;
  ~String () noexcept;

  String (const std::string &);
  String (const char *);
  String (const char *, std::size_t);
  String (const char16_t *);
  String (const char16_t *, std::size_t);

  static String lossy (const std::string &) noexcept;
  static String lossy (const char *) noexcept;
  static String lossy (const char *, std::size_t) noexcept;
  static String lossy (const char16_t *) noexcept;
  static String lossy (const char16_t *, std::size_t) noexcept;

  String &operator= (const String &) & noexcept;
  String &operator= (String &&) & noexcept;

  explicit operator std::string () const;

  const char *data () const noexcept;
  std::size_t size () const noexcept;
  std::size_t length () const noexcept;
  bool empty () const noexcept;

  const char *c_str () noexcept;

  std::size_t capacity () const noexcept;
  void reserve (size_t new_cap) noexcept;

  using iterator = char *;
  iterator begin () noexcept;
  iterator end () noexcept;

  using const_iterator = const char *;
  const_iterator begin () const noexcept;
  const_iterator end () const noexcept;
  const_iterator cbegin () const noexcept;
  const_iterator cend () const noexcept;

  bool operator== (const String &) const noexcept;
  bool operator!= (const String &) const noexcept;
  bool operator< (const String &) const noexcept;
  bool operator<= (const String &) const noexcept;
  bool operator> (const String &) const noexcept;
  bool operator>= (const String &) const noexcept;

  void swap (String &) noexcept;

  String (unsafe_bitcopy_t, const String &) noexcept;

private:
  struct lossy_t;
  String (lossy_t, const char *, std::size_t) noexcept;
  String (lossy_t, const char16_t *, std::size_t) noexcept;
  friend void
  swap (String &lhs, String &rhs) noexcept
  {
    lhs.swap (rhs);
  }

  std::array<std::uintptr_t, 3> repr;
};
#endif // CXXBRIDGE1_RUST_STRING

#ifndef CXXBRIDGE1_RUST_STR
#define CXXBRIDGE1_RUST_STR
class Str final
{
public:
  Str () noexcept;
  Str (const String &) noexcept;
  Str (const std::string &);
  Str (const char *);
  Str (const char *, std::size_t);

  Str &operator= (const Str &) &noexcept = default;

  explicit operator std::string () const;

  const char *data () const noexcept;
  std::size_t size () const noexcept;
  std::size_t length () const noexcept;
  bool empty () const noexcept;

  Str (const Str &) noexcept = default;
  ~Str () noexcept = default;

  using iterator = const char *;
  using const_iterator = const char *;
  const_iterator begin () const noexcept;
  const_iterator end () const noexcept;
  const_iterator cbegin () const noexcept;
  const_iterator cend () const noexcept;

  bool operator== (const Str &) const noexcept;
  bool operator!= (const Str &) const noexcept;
  bool operator< (const Str &) const noexcept;
  bool operator<= (const Str &) const noexcept;
  bool operator> (const Str &) const noexcept;
  bool operator>= (const Str &) const noexcept;

  void swap (Str &) noexcept;

private:
  class uninit;
  Str (uninit) noexcept;
  friend impl<Str>;

  std::array<std::uintptr_t, 2> repr;
};
#endif // CXXBRIDGE1_RUST_STR

#ifndef CXXBRIDGE1_RUST_SLICE
#define CXXBRIDGE1_RUST_SLICE
namespace detail
{
template <bool> struct copy_assignable_if
{
};

template <> struct copy_assignable_if<false>
{
  copy_assignable_if () noexcept = default;
  copy_assignable_if (const copy_assignable_if &) noexcept = default;
  copy_assignable_if &operator= (const copy_assignable_if &) &noexcept = delete;
  copy_assignable_if &operator= (copy_assignable_if &&) &noexcept = default;
};
} // namespace detail

template <typename T>
class Slice final : private detail::copy_assignable_if<std::is_const<T>::value>
{
public:
  using value_type = T;

  Slice () noexcept;
  Slice (T *, std::size_t count) noexcept;

  Slice &operator= (const Slice<T> &) &noexcept = default;
  Slice &operator= (Slice<T> &&) &noexcept = default;

  T *data () const noexcept;
  std::size_t size () const noexcept;
  std::size_t length () const noexcept;
  bool empty () const noexcept;

  T &operator[] (std::size_t n) const noexcept;
  T &at (std::size_t n) const;
  T &front () const noexcept;
  T &back () const noexcept;

  Slice (const Slice<T> &) noexcept = default;
  ~Slice () noexcept = default;

  class iterator;
  iterator begin () const noexcept;
  iterator end () const noexcept;

  void swap (Slice &) noexcept;

private:
  class uninit;
  Slice (uninit) noexcept;
  friend impl<Slice>;
  friend void sliceInit (void *, const void *, std::size_t) noexcept;
  friend void *slicePtr (const void *) noexcept;
  friend std::size_t sliceLen (const void *) noexcept;

  std::array<std::uintptr_t, 2> repr;
};

template <typename T> class Slice<T>::iterator final
{
public:
  using iterator_category = std::random_access_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = typename std::add_pointer<T>::type;
  using reference = typename std::add_lvalue_reference<T>::type;

  reference operator* () const noexcept;
  pointer operator->() const noexcept;
  reference operator[] (difference_type) const noexcept;

  iterator &operator++ () noexcept;
  iterator operator++ (int) noexcept;
  iterator &operator-- () noexcept;
  iterator operator-- (int) noexcept;

  iterator &operator+= (difference_type) noexcept;
  iterator &operator-= (difference_type) noexcept;
  iterator operator+ (difference_type) const noexcept;
  iterator operator- (difference_type) const noexcept;
  difference_type operator- (const iterator &) const noexcept;

  bool operator== (const iterator &) const noexcept;
  bool operator!= (const iterator &) const noexcept;
  bool operator< (const iterator &) const noexcept;
  bool operator<= (const iterator &) const noexcept;
  bool operator> (const iterator &) const noexcept;
  bool operator>= (const iterator &) const noexcept;

private:
  friend class Slice;
  void *pos;
  std::size_t stride;
};

template <typename T> Slice<T>::Slice () noexcept
{
  sliceInit (this, reinterpret_cast<void *> (align_of<T> ()), 0);
}

template <typename T> Slice<T>::Slice (T *s, std::size_t count) noexcept
{
  assert (s != nullptr || count == 0);
  sliceInit (this,
             s == nullptr && count == 0 ? reinterpret_cast<void *> (align_of<T> ())
                                        : const_cast<typename std::remove_const<T>::type *> (s),
             count);
}

template <typename T>
T *
Slice<T>::data () const noexcept
{
  return reinterpret_cast<T *> (slicePtr (this));
}

template <typename T>
std::size_t
Slice<T>::size () const noexcept
{
  return sliceLen (this);
}

template <typename T>
std::size_t
Slice<T>::length () const noexcept
{
  return this->size ();
}

template <typename T>
bool
Slice<T>::empty () const noexcept
{
  return this->size () == 0;
}

template <typename T>
T &
Slice<T>::operator[] (std::size_t n) const noexcept
{
  assert (n < this->size ());
  auto ptr = static_cast<char *> (slicePtr (this)) + size_of<T> () * n;
  return *reinterpret_cast<T *> (ptr);
}

template <typename T>
T &
Slice<T>::at (std::size_t n) const
{
  if (n >= this->size ())
    {
      panic<std::out_of_range> ("rust::Slice index out of range");
    }
  return (*this)[n];
}

template <typename T>
T &
Slice<T>::front () const noexcept
{
  assert (!this->empty ());
  return (*this)[0];
}

template <typename T>
T &
Slice<T>::back () const noexcept
{
  assert (!this->empty ());
  return (*this)[this->size () - 1];
}

template <typename T>
typename Slice<T>::iterator::reference
Slice<T>::iterator::operator* () const noexcept
{
  return *static_cast<T *> (this->pos);
}

template <typename T>
typename Slice<T>::iterator::pointer
Slice<T>::iterator::operator->() const noexcept
{
  return static_cast<T *> (this->pos);
}

template <typename T>
typename Slice<T>::iterator::reference
Slice<T>::iterator::operator[] (typename Slice<T>::iterator::difference_type n) const noexcept
{
  auto ptr = static_cast<char *> (this->pos) + this->stride * n;
  return *reinterpret_cast<T *> (ptr);
}

template <typename T>
typename Slice<T>::iterator &
Slice<T>::iterator::operator++ () noexcept
{
  this->pos = static_cast<char *> (this->pos) + this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::iterator::operator++ (int) noexcept
{
  auto ret = iterator (*this);
  this->pos = static_cast<char *> (this->pos) + this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &
Slice<T>::iterator::operator-- () noexcept
{
  this->pos = static_cast<char *> (this->pos) - this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::iterator::operator-- (int) noexcept
{
  auto ret = iterator (*this);
  this->pos = static_cast<char *> (this->pos) - this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &
Slice<T>::iterator::operator+= (typename Slice<T>::iterator::difference_type n) noexcept
{
  this->pos = static_cast<char *> (this->pos) + this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator &
Slice<T>::iterator::operator-= (typename Slice<T>::iterator::difference_type n) noexcept
{
  this->pos = static_cast<char *> (this->pos) - this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::iterator::operator+ (typename Slice<T>::iterator::difference_type n) const noexcept
{
  auto ret = iterator (*this);
  ret.pos = static_cast<char *> (this->pos) + this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::iterator::operator- (typename Slice<T>::iterator::difference_type n) const noexcept
{
  auto ret = iterator (*this);
  ret.pos = static_cast<char *> (this->pos) - this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator::difference_type
Slice<T>::iterator::operator- (const iterator &other) const noexcept
{
  auto diff = std::distance (static_cast<char *> (other.pos), static_cast<char *> (this->pos));
  return diff / static_cast<typename Slice<T>::iterator::difference_type> (this->stride);
}

template <typename T>
bool
Slice<T>::iterator::operator== (const iterator &other) const noexcept
{
  return this->pos == other.pos;
}

template <typename T>
bool
Slice<T>::iterator::operator!= (const iterator &other) const noexcept
{
  return this->pos != other.pos;
}

template <typename T>
bool
Slice<T>::iterator::operator< (const iterator &other) const noexcept
{
  return this->pos < other.pos;
}

template <typename T>
bool
Slice<T>::iterator::operator<= (const iterator &other) const noexcept
{
  return this->pos <= other.pos;
}

template <typename T>
bool
Slice<T>::iterator::operator> (const iterator &other) const noexcept
{
  return this->pos > other.pos;
}

template <typename T>
bool
Slice<T>::iterator::operator>= (const iterator &other) const noexcept
{
  return this->pos >= other.pos;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::begin () const noexcept
{
  iterator it;
  it.pos = slicePtr (this);
  it.stride = size_of<T> ();
  return it;
}

template <typename T>
typename Slice<T>::iterator
Slice<T>::end () const noexcept
{
  iterator it = this->begin ();
  it.pos = static_cast<char *> (it.pos) + it.stride * this->size ();
  return it;
}

template <typename T>
void
Slice<T>::swap (Slice &rhs) noexcept
{
  std::swap (*this, rhs);
}
#endif // CXXBRIDGE1_RUST_SLICE

#ifndef CXXBRIDGE1_RUST_BOX
#define CXXBRIDGE1_RUST_BOX
template <typename T> class Box final
{
public:
  using element_type = T;
  using const_pointer = typename std::add_pointer<typename std::add_const<T>::type>::type;
  using pointer = typename std::add_pointer<T>::type;

  Box () = delete;
  Box (Box &&) noexcept;
  ~Box () noexcept;

  explicit Box (const T &);
  explicit Box (T &&);

  Box &operator= (Box &&) & noexcept;

  const T *operator->() const noexcept;
  const T &operator* () const noexcept;
  T *operator->() noexcept;
  T &operator* () noexcept;

  template <typename... Fields> static Box in_place (Fields &&...);

  void swap (Box &) noexcept;

  static Box from_raw (T *) noexcept;

  T *into_raw () noexcept;

  /* Deprecated */ using value_type = element_type;

private:
  class uninit;
  class allocation;
  Box (uninit) noexcept;
  void drop () noexcept;

  friend void
  swap (Box &lhs, Box &rhs) noexcept
  {
    lhs.swap (rhs);
  }

  T *ptr;
};

template <typename T> class Box<T>::uninit
{
};

template <typename T> class Box<T>::allocation
{
  static T *alloc () noexcept;
  static void dealloc (T *) noexcept;

public:
  allocation () noexcept : ptr (alloc ()) {}
  ~allocation () noexcept
  {
    if (this->ptr)
      {
        dealloc (this->ptr);
      }
  }
  T *ptr;
};

template <typename T> Box<T>::Box (Box &&other) noexcept : ptr (other.ptr) { other.ptr = nullptr; }

template <typename T> Box<T>::Box (const T &val)
{
  allocation alloc;
  ::new (alloc.ptr) T (val);
  this->ptr = alloc.ptr;
  alloc.ptr = nullptr;
}

template <typename T> Box<T>::Box (T &&val)
{
  allocation alloc;
  ::new (alloc.ptr) T (std::move (val));
  this->ptr = alloc.ptr;
  alloc.ptr = nullptr;
}

template <typename T> Box<T>::~Box () noexcept
{
  if (this->ptr)
    {
      this->drop ();
    }
}

template <typename T>
    Box<T> &
    Box<T>::operator= (Box &&other)
    & noexcept
{
  if (this->ptr)
    {
      this->drop ();
    }
  this->ptr = other.ptr;
  other.ptr = nullptr;
  return *this;
}

template <typename T>
const T *
Box<T>::operator->() const noexcept
{
  return this->ptr;
}

template <typename T>
const T &
Box<T>::operator* () const noexcept
{
  return *this->ptr;
}

template <typename T>
T *
Box<T>::operator->() noexcept
{
  return this->ptr;
}

template <typename T>
T &
Box<T>::operator* () noexcept
{
  return *this->ptr;
}

template <typename T>
template <typename... Fields>
Box<T>
Box<T>::in_place (Fields &&...fields)
{
  allocation alloc;
  auto ptr = alloc.ptr;
  ::new (ptr) T{ std::forward<Fields> (fields)... };
  alloc.ptr = nullptr;
  return from_raw (ptr);
}

template <typename T>
void
Box<T>::swap (Box &rhs) noexcept
{
  using std::swap;
  swap (this->ptr, rhs.ptr);
}

template <typename T>
Box<T>
Box<T>::from_raw (T *raw) noexcept
{
  Box box = uninit{};
  box.ptr = raw;
  return box;
}

template <typename T>
T *
Box<T>::into_raw () noexcept
{
  T *raw = this->ptr;
  this->ptr = nullptr;
  return raw;
}

template <typename T> Box<T>::Box (uninit) noexcept {}
#endif // CXXBRIDGE1_RUST_BOX

#ifndef CXXBRIDGE1_RUST_BITCOPY_T
#define CXXBRIDGE1_RUST_BITCOPY_T
struct unsafe_bitcopy_t final
{
  explicit unsafe_bitcopy_t () = default;
};
#endif // CXXBRIDGE1_RUST_BITCOPY_T

#ifndef CXXBRIDGE1_RUST_VEC
#define CXXBRIDGE1_RUST_VEC
template <typename T> class Vec final
{
public:
  using value_type = T;

  Vec () noexcept;
  Vec (std::initializer_list<T>);
  Vec (const Vec &);
  Vec (Vec &&) noexcept;
  ~Vec () noexcept;

  Vec &operator= (Vec &&) & noexcept;
  Vec &operator= (const Vec &) &;

  std::size_t size () const noexcept;
  bool empty () const noexcept;
  const T *data () const noexcept;
  T *data () noexcept;
  std::size_t capacity () const noexcept;

  const T &operator[] (std::size_t n) const noexcept;
  const T &at (std::size_t n) const;
  const T &front () const noexcept;
  const T &back () const noexcept;

  T &operator[] (std::size_t n) noexcept;
  T &at (std::size_t n);
  T &front () noexcept;
  T &back () noexcept;

  void reserve (std::size_t new_cap);
  void push_back (const T &value);
  void push_back (T &&value);
  template <typename... Args> void emplace_back (Args &&...args);
  void truncate (std::size_t len);
  void clear ();

  using iterator = typename Slice<T>::iterator;
  iterator begin () noexcept;
  iterator end () noexcept;

  using const_iterator = typename Slice<const T>::iterator;
  const_iterator begin () const noexcept;
  const_iterator end () const noexcept;
  const_iterator cbegin () const noexcept;
  const_iterator cend () const noexcept;

  void swap (Vec &) noexcept;

  Vec (unsafe_bitcopy_t, const Vec &) noexcept;

private:
  void reserve_total (std::size_t new_cap) noexcept;
  void set_len (std::size_t len) noexcept;
  void drop () noexcept;

  friend void
  swap (Vec &lhs, Vec &rhs) noexcept
  {
    lhs.swap (rhs);
  }

  std::array<std::uintptr_t, 3> repr;
};

template <typename T> Vec<T>::Vec (std::initializer_list<T> init) : Vec{}
{
  this->reserve_total (init.size ());
  std::move (init.begin (), init.end (), std::back_inserter (*this));
}

template <typename T> Vec<T>::Vec (const Vec &other) : Vec ()
{
  this->reserve_total (other.size ());
  std::copy (other.begin (), other.end (), std::back_inserter (*this));
}

template <typename T> Vec<T>::Vec (Vec &&other) noexcept : repr (other.repr)
{
  new (&other) Vec ();
}

template <typename T> Vec<T>::~Vec () noexcept { this->drop (); }

template <typename T>
    Vec<T> &
    Vec<T>::operator= (Vec &&other)
    & noexcept
{
  this->drop ();
  this->repr = other.repr;
  new (&other) Vec ();
  return *this;
}

template <typename T>
Vec<T> &
Vec<T>::operator= (const Vec &other) &
{
  if (this != &other)
    {
      this->drop ();
      new (this) Vec (other);
    }
  return *this;
}

template <typename T>
bool
Vec<T>::empty () const noexcept
{
  return this->size () == 0;
}

template <typename T>
T *
Vec<T>::data () noexcept
{
  return const_cast<T *> (const_cast<const Vec<T> *> (this)->data ());
}

template <typename T>
const T &
Vec<T>::operator[] (std::size_t n) const noexcept
{
  assert (n < this->size ());
  auto data = reinterpret_cast<const char *> (this->data ());
  return *reinterpret_cast<const T *> (data + n * size_of<T> ());
}

template <typename T>
const T &
Vec<T>::at (std::size_t n) const
{
  if (n >= this->size ())
    {
      panic<std::out_of_range> ("rust::Vec index out of range");
    }
  return (*this)[n];
}

template <typename T>
const T &
Vec<T>::front () const noexcept
{
  assert (!this->empty ());
  return (*this)[0];
}

template <typename T>
const T &
Vec<T>::back () const noexcept
{
  assert (!this->empty ());
  return (*this)[this->size () - 1];
}

template <typename T>
T &
Vec<T>::operator[] (std::size_t n) noexcept
{
  assert (n < this->size ());
  auto data = reinterpret_cast<char *> (this->data ());
  return *reinterpret_cast<T *> (data + n * size_of<T> ());
}

template <typename T>
T &
Vec<T>::at (std::size_t n)
{
  if (n >= this->size ())
    {
      panic<std::out_of_range> ("rust::Vec index out of range");
    }
  return (*this)[n];
}

template <typename T>
T &
Vec<T>::front () noexcept
{
  assert (!this->empty ());
  return (*this)[0];
}

template <typename T>
T &
Vec<T>::back () noexcept
{
  assert (!this->empty ());
  return (*this)[this->size () - 1];
}

template <typename T>
void
Vec<T>::reserve (std::size_t new_cap)
{
  this->reserve_total (new_cap);
}

template <typename T>
void
Vec<T>::push_back (const T &value)
{
  this->emplace_back (value);
}

template <typename T>
void
Vec<T>::push_back (T &&value)
{
  this->emplace_back (std::move (value));
}

template <typename T>
template <typename... Args>
void
Vec<T>::emplace_back (Args &&...args)
{
  auto size = this->size ();
  this->reserve_total (size + 1);
  ::new (reinterpret_cast<T *> (reinterpret_cast<char *> (this->data ()) + size * size_of<T> ()))
      T (std::forward<Args> (args)...);
  this->set_len (size + 1);
}

template <typename T>
void
Vec<T>::clear ()
{
  this->truncate (0);
}

template <typename T>
typename Vec<T>::iterator
Vec<T>::begin () noexcept
{
  return Slice<T> (this->data (), this->size ()).begin ();
}

template <typename T>
typename Vec<T>::iterator
Vec<T>::end () noexcept
{
  return Slice<T> (this->data (), this->size ()).end ();
}

template <typename T>
typename Vec<T>::const_iterator
Vec<T>::begin () const noexcept
{
  return this->cbegin ();
}

template <typename T>
typename Vec<T>::const_iterator
Vec<T>::end () const noexcept
{
  return this->cend ();
}

template <typename T>
typename Vec<T>::const_iterator
Vec<T>::cbegin () const noexcept
{
  return Slice<const T> (this->data (), this->size ()).begin ();
}

template <typename T>
typename Vec<T>::const_iterator
Vec<T>::cend () const noexcept
{
  return Slice<const T> (this->data (), this->size ()).end ();
}

template <typename T>
void
Vec<T>::swap (Vec &rhs) noexcept
{
  using std::swap;
  swap (this->repr, rhs.repr);
}

template <typename T> Vec<T>::Vec (unsafe_bitcopy_t, const Vec &bits) noexcept : repr (bits.repr) {}
#endif // CXXBRIDGE1_RUST_VEC

#ifndef CXXBRIDGE1_RUST_OPAQUE
#define CXXBRIDGE1_RUST_OPAQUE
class Opaque
{
public:
  Opaque () = delete;
  Opaque (const Opaque &) = delete;
  ~Opaque () = delete;
};
#endif // CXXBRIDGE1_RUST_OPAQUE

#ifndef CXXBRIDGE1_IS_COMPLETE
#define CXXBRIDGE1_IS_COMPLETE
namespace detail
{
namespace
{
template <typename T, typename = std::size_t> struct is_complete : std::false_type
{
};
template <typename T> struct is_complete<T, decltype (sizeof (T))> : std::true_type
{
};
} // namespace
} // namespace detail
#endif // CXXBRIDGE1_IS_COMPLETE

#ifndef CXXBRIDGE1_LAYOUT
#define CXXBRIDGE1_LAYOUT
class layout
{
  template <typename T> friend std::size_t size_of ();
  template <typename T> friend std::size_t align_of ();
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value, std::size_t>::type
  do_size_of ()
  {
    return T::layout::size ();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value, std::size_t>::type
  do_size_of ()
  {
    return sizeof (T);
  }
  template <typename T>
  static typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
  size_of ()
  {
    return do_size_of<T> ();
  }
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value, std::size_t>::type
  do_align_of ()
  {
    return T::layout::align ();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value, std::size_t>::type
  do_align_of ()
  {
    return alignof (T);
  }
  template <typename T>
  static typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
  align_of ()
  {
    return do_align_of<T> ();
  }
};

template <typename T>
std::size_t
size_of ()
{
  return layout::size_of<T> ();
}

template <typename T>
std::size_t
align_of ()
{
  return layout::align_of<T> ();
}
#endif // CXXBRIDGE1_LAYOUT
} // namespace cxxbridge1
} // namespace rust

namespace rpmostreecxx
{
struct StringMapping;
enum class SystemHostType : ::std::uint8_t;
enum class BubblewrapMutability : ::std::uint8_t;
struct Bubblewrap;
struct ContainerImageState;
struct ExportedManifestDiff;
struct PrunedContainerInfo;
enum class RefspecType : ::std::uint8_t;
struct TempEtcGuard;
struct FilesystemScriptPrep;
struct DeploymentLayeredMeta;
struct OverrideReplacementSource;
enum class ParsedRevisionKind : ::std::uint8_t;
struct ParsedRevision;
struct RpmImporterFlags;
struct RpmImporter;
struct HistoryEntry;
struct HistoryCtx;
struct TokioHandle;
struct TokioEnterGuard;
enum class RepoMetadataTarget : ::std::uint8_t;
struct Refspec;
enum class OverrideReplacementType : ::std::uint8_t;
struct OverrideReplacement;
struct Treefile;
struct RepoPackage;
struct LiveApplyState;
struct PasswdDB;
struct PasswdEntries;
struct Extensions;
struct LockedPackage;
struct LockfileConfig;
using CxxGObjectArray = ::rpmostreecxx::CxxGObjectArray;
using ClientConnection = ::rpmostreecxx::ClientConnection;
using RPMDiff = ::rpmostreecxx::RPMDiff;
using RpmOstreeDiffPrintFormat = ::rpmostreecxx::RpmOstreeDiffPrintFormat;
using Progress = ::rpmostreecxx::Progress;
using RpmFileDb = ::rpmostreecxx::RpmFileDb;
using RpmTs = ::rpmostreecxx::RpmTs;
using PackageMeta = ::rpmostreecxx::PackageMeta;
}

namespace rpmostreecxx
{
#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$StringMapping
#define CXXBRIDGE1_STRUCT_rpmostreecxx$StringMapping
// Currently cxx-rs doesn't support mappings; like probably most projects,
// by far our most common case is a mapping from string -> string and since
// our data sizes aren't large, we serialize this as a vector of strings pairs.
// In the future it's also likely that cxx-rs will support a C++ string_view
// so we could avoid duplicating in that direction.
struct StringMapping final
{
  ::rust::String k;
  ::rust::String v;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$StringMapping

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$SystemHostType
#define CXXBRIDGE1_ENUM_rpmostreecxx$SystemHostType
// Classify the running system.
enum class SystemHostType : ::std::uint8_t
{
  OstreeContainer = 0,
  OstreeHost = 1,
  Unknown = 2,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$SystemHostType

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$BubblewrapMutability
#define CXXBRIDGE1_ENUM_rpmostreecxx$BubblewrapMutability
enum class BubblewrapMutability : ::std::uint8_t
{
  Immutable = 0,
  RoFiles = 1,
  MutateFreely = 2,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$BubblewrapMutability

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$Bubblewrap
#define CXXBRIDGE1_STRUCT_rpmostreecxx$Bubblewrap
struct Bubblewrap final : public ::rust::Opaque
{
  ::std::int32_t get_rootfs_fd () const noexcept;
  void append_bwrap_arg (::rust::Str arg) noexcept;
  void append_child_arg (::rust::Str arg) noexcept;
  void setenv (::rust::Str k, ::rust::Str v) noexcept;
  void take_fd (::std::int32_t source_fd, ::std::int32_t target_fd) noexcept;
  void set_inherit_stdin () noexcept;
  void take_stdin_fd (::std::int32_t source_fd) noexcept;
  void take_stdout_fd (::std::int32_t source_fd) noexcept;
  void take_stderr_fd (::std::int32_t source_fd) noexcept;
  void take_stdout_and_stderr_fd (::std::int32_t source_fd) noexcept;
  void bind_read (::rust::Str src, ::rust::Str dest) noexcept;
  void bind_readwrite (::rust::Str src, ::rust::Str dest) noexcept;
  void setup_compat_var ();
  void run (::rpmostreecxx::GCancellable const &cancellable);
  ~Bubblewrap () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$Bubblewrap

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$ExportedManifestDiff
#define CXXBRIDGE1_STRUCT_rpmostreecxx$ExportedManifestDiff
struct ExportedManifestDiff final
{
  // Check if the struct is initialized
  bool initialized;
  // The total number of packages in the next upgrade
  ::std::uint64_t total;
  // The size of the total number of packages in the next upgrade
  ::std::uint64_t total_size;
  // The total number of removed packages in the next upgrade
  ::std::uint64_t n_removed;
  // The size of total number of removed packages in the next upgrade
  ::std::uint64_t removed_size;
  // The total number of added packages in the next upgrade
  ::std::uint64_t n_added;
  // The size of total number of added packages in the next upgrade
  ::std::uint64_t added_size;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$ExportedManifestDiff

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$ContainerImageState
#define CXXBRIDGE1_STRUCT_rpmostreecxx$ContainerImageState
// `ContainerImageState` is currently identical to ostree-rs-ext's `LayeredImageState` struct,
// because cxx.rs currently requires types used as extern Rust types to be defined by the same crate
// that contains the bridge using them, so we redefine an `ContainerImport` struct here.
struct ContainerImageState final
{
  ::rust::String base_commit;
  ::rust::String merge_commit;
  bool is_layered;
  ::rust::String image_digest;
  ::rust::String version;
  ::rpmostreecxx::ExportedManifestDiff cached_update_diff;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$ContainerImageState

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$PrunedContainerInfo
#define CXXBRIDGE1_STRUCT_rpmostreecxx$PrunedContainerInfo
struct PrunedContainerInfo final
{
  ::std::uint32_t images;
  ::std::uint32_t layers;

  bool operator== (PrunedContainerInfo const &) const noexcept;
  bool operator!= (PrunedContainerInfo const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$PrunedContainerInfo

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$RefspecType
#define CXXBRIDGE1_ENUM_rpmostreecxx$RefspecType
enum class RefspecType : ::std::uint8_t
{
  Ostree = 0,
  Checksum = 1,
  Container = 2,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$RefspecType

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$TempEtcGuard
#define CXXBRIDGE1_STRUCT_rpmostreecxx$TempEtcGuard
struct TempEtcGuard final : public ::rust::Opaque
{
  void undo () const;
  ~TempEtcGuard () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$TempEtcGuard

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$FilesystemScriptPrep
#define CXXBRIDGE1_STRUCT_rpmostreecxx$FilesystemScriptPrep
struct FilesystemScriptPrep final : public ::rust::Opaque
{
  void undo ();
  ~FilesystemScriptPrep () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$FilesystemScriptPrep

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$DeploymentLayeredMeta
#define CXXBRIDGE1_STRUCT_rpmostreecxx$DeploymentLayeredMeta
struct DeploymentLayeredMeta final
{
  bool is_layered;
  ::rust::String base_commit;
  ::std::uint32_t clientlayer_version;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$DeploymentLayeredMeta

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacementSource
#define CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacementSource
struct OverrideReplacementSource final
{
  ::rpmostreecxx::OverrideReplacementType kind;
  ::rust::String name;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacementSource

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$ParsedRevisionKind
#define CXXBRIDGE1_ENUM_rpmostreecxx$ParsedRevisionKind
enum class ParsedRevisionKind : ::std::uint8_t
{
  Version = 0,
  Checksum = 1,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$ParsedRevisionKind

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$ParsedRevision
#define CXXBRIDGE1_STRUCT_rpmostreecxx$ParsedRevision
struct ParsedRevision final
{
  ::rpmostreecxx::ParsedRevisionKind kind;
  ::rust::String value;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$ParsedRevision

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporterFlags
#define CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporterFlags
struct RpmImporterFlags final : public ::rust::Opaque
{
  bool is_ima_enabled () const noexcept;
  ~RpmImporterFlags () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporterFlags

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporter
#define CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporter
struct RpmImporter final : public ::rust::Opaque
{
  ::rust::String handle_translate_pathname (::rust::Str path) noexcept;
  ::rust::String ostree_branch () const noexcept;
  ::rust::String pkg_name () const noexcept;
  bool doc_files_are_filtered () const noexcept;
  void doc_files_insert (::rust::Str path) noexcept;
  bool doc_files_contains (::rust::Str path) const noexcept;
  void rpmfi_overrides_insert (::rust::Str path, ::std::uint64_t index) noexcept;
  bool rpmfi_overrides_contains (::rust::Str path) const noexcept;
  ::std::uint64_t rpmfi_overrides_get (::rust::Str path) const noexcept;
  bool is_ima_enabled () const noexcept;
  void tweak_imported_file_info (::rpmostreecxx::GFileInfo const &file_info) const noexcept;
  bool is_file_filtered (::rust::Str path, ::rpmostreecxx::GFileInfo const &file_info) const;
  void translate_to_tmpfiles_entry (::rust::Str abs_path,
                                    ::rpmostreecxx::GFileInfo const &file_info,
                                    ::rust::Str username, ::rust::Str groupname);
  bool has_tmpfiles_entries () const noexcept;
  ::rust::String serialize_tmpfiles_content () const noexcept;
  ~RpmImporter () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$RpmImporter

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryEntry
#define CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryEntry
// A history entry in the journal. It may represent multiple consecutive boots
// into the same deployment. This struct is exposed directly via FFI to C.
struct HistoryEntry final
{
  // The deployment root timestamp.
  ::std::uint64_t deploy_timestamp;
  // The command-line that was used to create the deployment, if any.
  ::rust::String deploy_cmdline;
  // The number of consecutive times the deployment was booted.
  ::std::uint64_t boot_count;
  // The first time the deployment was booted if multiple consecutive times.
  ::std::uint64_t first_boot_timestamp;
  // The last time the deployment was booted if multiple consecutive times.
  ::std::uint64_t last_boot_timestamp;
  // `true` if there are no more entries.
  bool eof;

  bool operator== (HistoryEntry const &) const noexcept;
  bool operator!= (HistoryEntry const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryEntry

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryCtx
#define CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryCtx
struct HistoryCtx final : public ::rust::Opaque
{
  ::rpmostreecxx::HistoryEntry next_entry ();
  ~HistoryCtx () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$HistoryCtx

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$TokioHandle
#define CXXBRIDGE1_STRUCT_rpmostreecxx$TokioHandle
struct TokioHandle final : public ::rust::Opaque
{
  ::rust::Box< ::rpmostreecxx::TokioEnterGuard> enter () const noexcept;
  ~TokioHandle () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$TokioHandle

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$TokioEnterGuard
#define CXXBRIDGE1_STRUCT_rpmostreecxx$TokioEnterGuard
struct TokioEnterGuard final : public ::rust::Opaque
{
  ~TokioEnterGuard () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$TokioEnterGuard

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$RepoMetadataTarget
#define CXXBRIDGE1_ENUM_rpmostreecxx$RepoMetadataTarget
enum class RepoMetadataTarget : ::std::uint8_t
{
  Inline = 0,
  Detached = 1,
  Disabled = 2,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$RepoMetadataTarget

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$Refspec
#define CXXBRIDGE1_STRUCT_rpmostreecxx$Refspec
struct Refspec final
{
  ::rpmostreecxx::RefspecType kind;
  ::rust::String refspec;

  bool operator== (Refspec const &) const noexcept;
  bool operator!= (Refspec const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$Refspec

#ifndef CXXBRIDGE1_ENUM_rpmostreecxx$OverrideReplacementType
#define CXXBRIDGE1_ENUM_rpmostreecxx$OverrideReplacementType
enum class OverrideReplacementType : ::std::uint8_t
{
  Repo = 0,
};
#endif // CXXBRIDGE1_ENUM_rpmostreecxx$OverrideReplacementType

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacement
#define CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacement
struct OverrideReplacement final
{
  ::rust::String from;
  ::rpmostreecxx::OverrideReplacementType from_kind;
  ::rust::Vec< ::rust::String> packages;

  bool operator== (OverrideReplacement const &) const noexcept;
  bool operator!= (OverrideReplacement const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$OverrideReplacement

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$Treefile
#define CXXBRIDGE1_STRUCT_rpmostreecxx$Treefile
struct Treefile final : public ::rust::Opaque
{
  ::rust::Str get_workdir () const noexcept;
  ::std::int32_t get_passwd_fd () noexcept;
  ::std::int32_t get_group_fd () noexcept;
  ::rust::String get_json_string () const noexcept;
  ::rust::Vec< ::rust::String> get_ostree_layers () const noexcept;
  ::rust::Vec< ::rust::String> get_ostree_override_layers () const noexcept;
  ::rust::Vec< ::rust::String> get_all_ostree_layers () const noexcept;
  ::rust::Vec< ::rust::String> get_repos () const noexcept;
  ::rust::Vec< ::rust::String> get_packages () const noexcept;
  ::rust::String require_automatic_version_prefix () const;
  bool add_packages (::rust::Vec< ::rust::String> packages, bool allow_existing);
  bool has_packages () const noexcept;
  ::rust::Vec< ::rust::String> get_local_packages () const noexcept;
  bool add_local_packages (::rust::Vec< ::rust::String> packages, bool allow_existing);
  ::rust::Vec< ::rust::String> get_local_fileoverride_packages () const noexcept;
  bool add_local_fileoverride_packages (::rust::Vec< ::rust::String> packages, bool allow_existing);
  bool remove_packages (::rust::Vec< ::rust::String> packages, bool allow_noent);
  ::rust::Vec< ::rpmostreecxx::OverrideReplacement> get_packages_override_replace () const noexcept;
  bool has_packages_override_replace () const noexcept;
  bool add_packages_override_replace (::rpmostreecxx::OverrideReplacement replacement) noexcept;
  bool remove_package_override_replace (::rust::Str package) noexcept;
  ::rust::Vec< ::rust::String> get_packages_override_replace_local () const noexcept;
  void add_packages_override_replace_local (::rust::Vec< ::rust::String> packages);
  bool remove_package_override_replace_local (::rust::Str package) noexcept;
  ::rust::Vec< ::rust::String> get_packages_override_remove () const noexcept;
  void add_packages_override_remove (::rust::Vec< ::rust::String> packages);
  bool remove_package_override_remove (::rust::Str package) noexcept;
  bool has_packages_override_remove_name (::rust::Str name) const noexcept;
  bool remove_all_overrides () noexcept;
  ::rust::Vec< ::rust::String> get_modules_enable () const noexcept;
  bool has_modules_enable () const noexcept;
  ::rust::Vec< ::rust::String> get_modules_install () const noexcept;
  bool add_modules (::rust::Vec< ::rust::String> modules, bool enable_only) noexcept;
  bool remove_modules (::rust::Vec< ::rust::String> modules, bool enable_only) noexcept;
  bool remove_all_packages () noexcept;
  ::rust::Vec< ::rust::String> get_exclude_packages () const noexcept;
  ::rust::String get_platform_module () const noexcept;
  ::rust::Vec< ::rust::String> get_install_langs () const noexcept;
  ::rust::String format_install_langs_macro () const noexcept;
  ::rust::Vec< ::rust::String> get_lockfile_repos () const noexcept;
  ::rust::Str get_ref () const noexcept;
  bool get_cliwrap () const noexcept;
  ::rust::Vec< ::rust::String> get_cliwrap_binaries () const noexcept;
  void set_cliwrap (bool enabled) noexcept;
  ::rust::Vec< ::rust::String> get_container_cmd () const noexcept;
  bool get_readonly_executables () const noexcept;
  bool get_documentation () const noexcept;
  bool get_recommends () const noexcept;
  bool get_selinux () const noexcept;
  ::std::uint32_t get_selinux_label_version () const noexcept;
  ::rust::String get_gpg_key () const noexcept;
  ::rust::String get_automatic_version_suffix () const noexcept;
  bool get_container () const noexcept;
  bool get_machineid_compat () const noexcept;
  ::rust::Vec< ::rust::String> get_etc_group_members () const noexcept;
  bool get_boot_location_is_modules () const noexcept;
  bool get_ima () const noexcept;
  ::rust::String get_releasever () const noexcept;
  ::rpmostreecxx::RepoMetadataTarget get_repo_metadata_target () const noexcept;
  bool rpmdb_backend_is_target () const noexcept;
  bool should_normalize_rpmdb () const noexcept;
  ::rust::Vec< ::rust::String> get_files_remove_regex (::rust::Str package) const noexcept;
  ::rust::String get_checksum (::rpmostreecxx::OstreeRepo const &repo) const;
  ::rust::String get_ostree_ref () const noexcept;
  ::rust::Slice< ::rpmostreecxx::RepoPackage const> get_repo_packages () const noexcept;
  void clear_repo_packages () noexcept;
  void prettyprint_json_stdout () const noexcept;
  void print_deprecation_warnings () const noexcept;
  void print_experimental_notices () const noexcept;
  void sanitycheck_externals () const;
  ::rust::Box< ::rpmostreecxx::RpmImporterFlags>
  importer_flags (::rust::Str pkg_name) const noexcept;
  ::rust::String write_repovars (::std::int32_t workdir_dfd_raw) const;
  void set_releasever (::rust::Str releasever);
  void enable_repo (::rust::Str repo);
  void disable_repo (::rust::Str repo);
  void validate_for_container () const;
  ::rpmostreecxx::Refspec get_base_refspec () const noexcept;
  void rebase (::rust::Str new_refspec, ::rust::Str custom_origin_url,
               ::rust::Str custom_origin_description) noexcept;
  ::rust::String get_origin_custom_url () const noexcept;
  ::rust::String get_origin_custom_description () const noexcept;
  ::rust::String get_override_commit () const noexcept;
  void set_override_commit (::rust::Str checksum) noexcept;
  ::rust::Vec< ::rust::String> get_initramfs_etc_files () const noexcept;
  bool has_initramfs_etc_files () const noexcept;
  bool initramfs_etc_files_track (::rust::Vec< ::rust::String> files) noexcept;
  bool initramfs_etc_files_untrack (::rust::Vec< ::rust::String> files) noexcept;
  bool initramfs_etc_files_untrack_all () noexcept;
  bool get_initramfs_regenerate () const noexcept;
  ::rust::Vec< ::rust::String> get_initramfs_args () const noexcept;
  void set_initramfs_regenerate (bool enabled, ::rust::Vec< ::rust::String> args) noexcept;
  ::rust::String get_unconfigured_state () const noexcept;
  bool may_require_local_assembly () const noexcept;
  bool has_any_packages () const noexcept;
  bool merge_treefile (::rust::Str treefile);
  ~Treefile () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$Treefile

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$RepoPackage
#define CXXBRIDGE1_STRUCT_rpmostreecxx$RepoPackage
struct RepoPackage final : public ::rust::Opaque
{
  ::rust::Str get_repo () const noexcept;
  ::rust::Vec< ::rust::String> get_packages () const noexcept;
  ~RepoPackage () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$RepoPackage

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$LiveApplyState
#define CXXBRIDGE1_STRUCT_rpmostreecxx$LiveApplyState
// A copy of LiveFsState that is bridged to C++; the main
// change here is we can't use Option<> yet, so empty values
// are represented by the empty string.
struct LiveApplyState final
{
  ::rust::String inprogress;
  ::rust::String commit;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$LiveApplyState

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdDB
#define CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdDB
struct PasswdDB final : public ::rust::Opaque
{
  ::rust::String lookup_user (::std::uint32_t uid) const;
  ::rust::String lookup_group (::std::uint32_t gid) const;
  ~PasswdDB () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdDB

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdEntries
#define CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdEntries
struct PasswdEntries final : public ::rust::Opaque
{
  void add_group_content (::std::int32_t rootfs, ::rust::Str path);
  void add_passwd_content (::std::int32_t rootfs, ::rust::Str path);
  bool contains_group (::rust::Str user) const noexcept;
  bool contains_user (::rust::Str user) const noexcept;
  ::std::uint32_t lookup_user_id (::rust::Str user) const;
  ::std::uint32_t lookup_group_id (::rust::Str group) const;
  ~PasswdEntries () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$PasswdEntries

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$Extensions
#define CXXBRIDGE1_STRUCT_rpmostreecxx$Extensions
struct Extensions final : public ::rust::Opaque
{
  ::rust::Vec< ::rust::String> get_repos () const noexcept;
  ::rust::Vec< ::rust::String> get_os_extension_packages () const noexcept;
  ::rust::Vec< ::rust::String> get_development_packages () const noexcept;
  bool state_checksum_changed (::rust::Str chksum, ::rust::Str output_dir) const;
  void update_state_checksum (::rust::Str chksum, ::rust::Str output_dir) const;
  void serialize_to_dir (::rust::Str output_dir) const;
  ::rust::Box< ::rpmostreecxx::Treefile>
  generate_treefile (::rpmostreecxx::Treefile const &src) const;
  ~Extensions () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$Extensions

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$LockedPackage
#define CXXBRIDGE1_STRUCT_rpmostreecxx$LockedPackage
struct LockedPackage final
{
  ::rust::String name;
  ::rust::String evr;
  ::rust::String arch;
  ::rust::String digest;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$LockedPackage

#ifndef CXXBRIDGE1_STRUCT_rpmostreecxx$LockfileConfig
#define CXXBRIDGE1_STRUCT_rpmostreecxx$LockfileConfig
struct LockfileConfig final : public ::rust::Opaque
{
  ::rust::Vec< ::rpmostreecxx::LockedPackage> get_locked_packages () const;
  ~LockfileConfig () = delete;

private:
  friend ::rust::layout;
  struct layout
  {
    static ::std::size_t size () noexcept;
    static ::std::size_t align () noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$LockfileConfig

static_assert (::std::is_enum<RpmOstreeDiffPrintFormat>::value, "expected enum");
static_assert (sizeof (RpmOstreeDiffPrintFormat) == sizeof (::std::uint8_t), "incorrect size");
static_assert (
    static_cast< ::std::uint8_t> (RpmOstreeDiffPrintFormat::RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY)
        == 0,
    "disagrees with the value in #[cxx::bridge]");
static_assert (static_cast< ::std::uint8_t> (
                   RpmOstreeDiffPrintFormat::RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED)
                   == 1,
               "disagrees with the value in #[cxx::bridge]");
static_assert (static_cast< ::std::uint8_t> (
                   RpmOstreeDiffPrintFormat::RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE)
                   == 2,
               "disagrees with the value in #[cxx::bridge]");

bool is_bare_split_xattrs ();

bool is_http_arg (::rust::Str arg) noexcept;

bool is_ostree_container ();

::rpmostreecxx::SystemHostType get_system_host_type ();

void require_system_host_type (::rpmostreecxx::SystemHostType t);

bool is_rpm_arg (::rust::Str arg) noexcept;

void client_start_daemon ();

::rust::Vec< ::std::int32_t> client_handle_fd_argument (::rust::Str arg, ::rust::Str arch,
                                                        bool is_replace);

::rust::String client_render_download_progress (::rpmostreecxx::GVariant const &progress) noexcept;

bool running_in_container () noexcept;

bool confirm ();

void confirm_or_abort ();

void bubblewrap_selftest ();

::rust::Vec< ::std::uint8_t> bubblewrap_run_sync (::std::int32_t rootfs_dfd,
                                                  ::rust::Vec< ::rust::String> const &args,
                                                  bool capture_stdout, bool unified_core);

::rust::Box< ::rpmostreecxx::Bubblewrap> bubblewrap_new (::std::int32_t rootfs_fd);

::rust::Box< ::rpmostreecxx::Bubblewrap>
bubblewrap_new_with_mutability (::std::int32_t rootfs_fd,
                                ::rpmostreecxx::BubblewrapMutability mutability);

void usroverlay_entrypoint (::rust::Vec< ::rust::String> const &args);

void applylive_entrypoint (::rust::Vec< ::rust::String> const &args);

void applylive_finish (::rpmostreecxx::OstreeSysroot const &sysroot);

void composeutil_legacy_prep_dev_and_run (::std::int32_t rootfs_dfd);

void print_ostree_txn_stats (::rpmostreecxx::OstreeRepoTransactionStats &stats) noexcept;

void write_commit_id (::rust::Str target_path, ::rust::Str revision);

void cliwrap_write_wrappers (::std::int32_t rootfs);

void cliwrap_write_some_wrappers (::std::int32_t rootfs, ::rust::Vec< ::rust::String> const &args);

::rust::String cliwrap_destdir () noexcept;

void container_encapsulate (::rust::Vec< ::rust::String> args);

void deploy_from_self_entrypoint (::rust::Vec< ::rust::String> args);

::rust::Box< ::rpmostreecxx::ContainerImageState>
pull_container (::rpmostreecxx::OstreeRepo const &repo,
                ::rpmostreecxx::GCancellable const &cancellable, ::rust::Str imgref);

::rpmostreecxx::PrunedContainerInfo container_prune (::rpmostreecxx::OstreeSysroot const &sysroot);

::rust::Box< ::rpmostreecxx::ContainerImageState>
query_container_image_commit (::rpmostreecxx::OstreeRepo const &repo, ::rust::Str c);

void purge_refspec (::rpmostreecxx::OstreeRepo const &repo, ::rust::Str refspec);

bool check_container_update (::rpmostreecxx::OstreeRepo const &repo,
                             ::rpmostreecxx::GCancellable const &cancellable, ::rust::Str imgref);

::rust::Box< ::rpmostreecxx::TempEtcGuard> prepare_tempetc_guard (::std::int32_t rootfs);

::rust::Box< ::rpmostreecxx::FilesystemScriptPrep>
prepare_filesystem_script_prep (::std::int32_t rootfs);

void run_depmod (::std::int32_t rootfs_dfd, ::rust::Str kver, bool unified_core);

void log_treefile (::rpmostreecxx::Treefile const &tf) noexcept;

bool is_container_image_reference (::rust::Str refspec) noexcept;

::rpmostreecxx::RefspecType refspec_classify (::rust::Str refspec) noexcept;

void verify_kernel_hmac (::std::int32_t rootfs, ::rust::Str moddir);

::rust::Vec< ::rust::String> stage_container_rpms (::rust::Vec< ::rust::String> rpms);

::rust::Vec< ::rust::String> stage_container_rpm_raw_fds (::rust::Vec< ::std::int32_t> fds);

bool commit_has_matching_sepolicy (::rpmostreecxx::GVariant const &commit,
                                   ::rpmostreecxx::OstreeSePolicy const &policy);

::rpmostreecxx::GVariant *get_header_variant (::rpmostreecxx::OstreeRepo const &repo,
                                              ::rust::Str cachebranch);

void compose_image (::rust::Vec< ::rust::String> args);

void configure_build_repo_from_target (::rpmostreecxx::OstreeRepo const &build_repo,
                                       ::rpmostreecxx::OstreeRepo const &target_repo);

void compose_prepare_rootfs (::std::int32_t src_rootfs_dfd, ::std::int32_t dest_rootfs_dfd,
                             ::rpmostreecxx::Treefile &treefile);

void composepost_nsswitch_altfiles (::std::int32_t rootfs_dfd);

void compose_postprocess (::std::int32_t rootfs_dfd, ::rpmostreecxx::Treefile &treefile,
                          ::rust::Str next_version, bool unified_core);

void compose_postprocess_final_pre (::std::int32_t rootfs_dfd);

void compose_postprocess_final (::std::int32_t rootfs_dfd,
                                ::rpmostreecxx::Treefile const &treefile);

void convert_var_to_tmpfiles_d (::std::int32_t rootfs_dfd,
                                ::rpmostreecxx::GCancellable const &cancellable);

void rootfs_prepare_links (::std::int32_t rootfs_dfd);

void workaround_selinux_cross_labeling (::std::int32_t rootfs_dfd,
                                        ::rpmostreecxx::GCancellable &cancellable);

void compose_postprocess_rpm_macro (::std::int32_t rootfs_dfd);

void postprocess_cleanup_rpmdb (::std::int32_t rootfs_dfd);

void rewrite_rpmdb_for_target (::std::int32_t rootfs_dfd, bool normalize);

::std::uint64_t directory_size (::std::int32_t dfd,
                                ::rpmostreecxx::GCancellable const &cancellable);

::rpmostreecxx::OstreeDeployment *deployment_for_id (::rpmostreecxx::OstreeSysroot &sysroot,
                                                     ::rust::Str deploy_id);

::rust::String deployment_checksum_for_id (::rpmostreecxx::OstreeSysroot &sysroot,
                                           ::rust::Str deploy_id);

::rpmostreecxx::OstreeDeployment *deployment_get_base (::rpmostreecxx::OstreeSysroot &sysroot,
                                                       ::rust::Str opt_deploy_id,
                                                       ::rust::Str opt_os_name);

bool deployment_add_manifest_diff (::rpmostreecxx::GVariantDict const &dict,
                                   ::rpmostreecxx::ExportedManifestDiff const &diff) noexcept;

void daemon_sanitycheck_environment (::rpmostreecxx::OstreeSysroot const &sysroot);

::rust::String deployment_generate_id (::rpmostreecxx::OstreeDeployment const &deployment) noexcept;

void deployment_populate_variant (::rpmostreecxx::OstreeSysroot const &sysroot,
                                  ::rpmostreecxx::OstreeDeployment const &deployment,
                                  ::rpmostreecxx::GVariantDict const &dict);

void generate_baselayer_refs (::rpmostreecxx::OstreeSysroot const &sysroot,
                              ::rpmostreecxx::OstreeRepo const &repo,
                              ::rpmostreecxx::GCancellable const &cancellable);

void variant_add_remote_status (::rpmostreecxx::OstreeRepo const &repo, ::rust::Str refspec,
                                ::rust::Str base_checksum,
                                ::rpmostreecxx::GVariantDict const &dict);

::rpmostreecxx::DeploymentLayeredMeta
deployment_layeredmeta_from_commit (::rpmostreecxx::OstreeDeployment const &deployment,
                                    ::rpmostreecxx::GVariant const &commit);

::rpmostreecxx::DeploymentLayeredMeta
deployment_layeredmeta_load (::rpmostreecxx::OstreeRepo const &repo,
                             ::rpmostreecxx::OstreeDeployment const &deployment);

::rpmostreecxx::OverrideReplacementSource parse_override_source (::rust::Str source);

::rpmostreecxx::ParsedRevision parse_revision (::rust::Str source);

::rust::String generate_object_path (::rust::Str base, ::rust::Str next_segment);

void failpoint (::rust::Str p);

::rust::Box< ::rpmostreecxx::RpmImporterFlags> rpm_importer_flags_new_empty () noexcept;

::rust::Box< ::rpmostreecxx::RpmImporter>
rpm_importer_new (::rust::Str pkg_name, ::rust::Str ostree_branch,
                  ::rpmostreecxx::RpmImporterFlags const &flags);

::rust::String tmpfiles_translate (::rust::Str abs_path, ::rpmostreecxx::GFileInfo const &file_info,
                                   ::rust::Str username, ::rust::Str groupname);

void append_dracut_random_cpio (::std::int32_t fd);

::std::int32_t initramfs_overlay_generate (::rust::Vec< ::rust::String> const &files,
                                           ::rpmostreecxx::GCancellable &cancellable);

void journal_print_staging_failure () noexcept;

void console_progress_begin_task (::rust::Str msg) noexcept;

void console_progress_begin_n_items (::rust::Str msg, ::std::uint64_t n) noexcept;

void console_progress_begin_percent (::rust::Str msg) noexcept;

void console_progress_set_message (::rust::Str msg) noexcept;

void console_progress_set_sub_message (::rust::Str msg) noexcept;

void console_progress_update (::std::uint64_t n) noexcept;

void console_progress_end (::rust::Str suffix) noexcept;

::rust::Box< ::rpmostreecxx::HistoryCtx> history_ctx_new ();

void history_prune ();

void modularity_entrypoint (::rust::Vec< ::rust::String> const &args);

::rust::Box< ::rpmostreecxx::TokioHandle> tokio_handle_get () noexcept;

bool script_is_ignored (::rust::Str pkg, ::rust::Str script) noexcept;

void testutils_entrypoint (::rust::Vec< ::rust::String> argv);

::rust::String maybe_shell_quote (::rust::Str input) noexcept;

::rust::Box< ::rpmostreecxx::Treefile> treefile_new (::rust::Str filename, ::rust::Str basearch);

::rust::Box< ::rpmostreecxx::Treefile> treefile_new_empty ();

::rust::Box< ::rpmostreecxx::Treefile> treefile_new_from_string (::rust::Str buf, bool client);

::rust::Box< ::rpmostreecxx::Treefile> treefile_new_compose (::rust::Str filename,
                                                             ::rust::Str basearch);

::rust::Box< ::rpmostreecxx::Treefile> treefile_new_client (::rust::Str filename,
                                                            ::rust::Str basearch);

::rust::Box< ::rpmostreecxx::Treefile> treefile_new_client_from_etc (::rust::Str basearch);

::std::uint32_t treefile_delete_client_etc ();

::rust::String varsubstitute (::rust::Str s,
                              ::rust::Vec< ::rpmostreecxx::StringMapping> const &vars);

::rust::Vec< ::rust::String> get_features () noexcept;

::rust::String get_rpm_basearch () noexcept;

::std::int32_t sealed_memfd (::rust::Str description, ::rust::Slice< ::std::uint8_t const> content);

bool running_in_systemd () noexcept;

::rpmostreecxx::GVariant *calculate_advisories_diff (::rpmostreecxx::OstreeRepo const &repo,
                                                     ::rust::Str checksum_from,
                                                     ::rust::Str checksum_to);

::rust::String translate_path_for_ostree (::rust::Str path) noexcept;

::rpmostreecxx::LiveApplyState
get_live_apply_state (::rpmostreecxx::OstreeSysroot const &sysroot,
                      ::rpmostreecxx::OstreeDeployment const &deployment);

bool has_live_apply_state (::rpmostreecxx::OstreeSysroot const &sysroot,
                           ::rpmostreecxx::OstreeDeployment const &deployment);

void applylive_sync_ref (::rpmostreecxx::OstreeSysroot const &sysroot);

void transaction_apply_live (::rpmostreecxx::OstreeSysroot const &sysroot,
                             ::rpmostreecxx::GVariant const &target);

bool prepare_rpm_layering (::std::int32_t rootfs, ::rust::Str merge_passwd_dir);

void complete_rpm_layering (::std::int32_t rootfs);

void deduplicate_tmpfiles_entries (::std::int32_t rootfs);

void passwd_cleanup (::std::int32_t rootfs);

void migrate_group_except_root (::std::int32_t rootfs,
                                ::rust::Vec< ::rust::String> const &preserved_groups);

void migrate_passwd_except_root (::std::int32_t rootfs);

void passwd_compose_prep (::std::int32_t rootfs, ::rpmostreecxx::Treefile &treefile);

void passwd_compose_prep_repo (::std::int32_t rootfs, ::rpmostreecxx::Treefile &treefile,
                               ::rpmostreecxx::OstreeRepo const &repo,
                               ::rust::Str previous_checksum, bool unified_core);

bool dir_contains_uid (::std::int32_t dirfd, ::std::uint32_t id);

bool dir_contains_gid (::std::int32_t dirfd, ::std::uint32_t id);

void check_passwd_group_entries (::rpmostreecxx::OstreeRepo const &ffi_repo,
                                 ::std::int32_t rootfs_dfd, ::rpmostreecxx::Treefile &treefile,
                                 ::rust::Str previous_rev);

::rust::Box< ::rpmostreecxx::PasswdDB> passwddb_open (::std::int32_t rootfs);

::rust::Box< ::rpmostreecxx::PasswdEntries> new_passwd_entries () noexcept;

::rust::Box< ::rpmostreecxx::Extensions>
extensions_load (::rust::Str path, ::rust::Str basearch,
                 ::rust::Vec< ::rpmostreecxx::StringMapping> const &base_pkgs);

::rust::Box< ::rpmostreecxx::LockfileConfig>
lockfile_read (::rust::Vec< ::rust::String> const &filenames);

void lockfile_write (::rust::Str filename, ::rpmostreecxx::CxxGObjectArray &packages,
                     ::rpmostreecxx::CxxGObjectArray &rpmmd_repos);

::rust::Box< ::rpmostreecxx::Treefile> origin_to_treefile (::rpmostreecxx::GKeyFile const &kf);

::rpmostreecxx::GKeyFile *treefile_to_origin (::rpmostreecxx::Treefile const &tf);

void origin_validate_roundtrip (::rpmostreecxx::GKeyFile const &kf) noexcept;

::rust::String cache_branch_to_nevra (::rust::Str nevra) noexcept;
} // namespace rpmostreecxx
