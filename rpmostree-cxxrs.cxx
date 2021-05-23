#include "rpmostree-cxxrs.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-container.hpp"
#include "rpmostree-cxxrsutil.hpp"
#include "rpmostree-diff.hpp"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-output.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include "rpmostreed-daemon.hpp"
#include "rpmostreemain.h"
#include "src/libpriv/rpmostree-cxxrs-prelude.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
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
  return diff / this->stride;
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

#ifndef CXXBRIDGE1_RUST_BITCOPY
#define CXXBRIDGE1_RUST_BITCOPY
constexpr unsafe_bitcopy_t unsafe_bitcopy{};
#endif // CXXBRIDGE1_RUST_BITCOPY

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

#ifndef CXXBRIDGE1_RUST_ERROR
#define CXXBRIDGE1_RUST_ERROR
class Error final : public std::exception
{
public:
  Error (const Error &);
  Error (Error &&) noexcept;
  ~Error () noexcept override;

  Error &operator= (const Error &) &;
  Error &operator= (Error &&) & noexcept;

  const char *what () const noexcept override;

private:
  Error () noexcept = default;
  friend impl<Error>;
  const char *msg;
  std::size_t len;
};
#endif // CXXBRIDGE1_RUST_ERROR

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

#ifndef CXXBRIDGE1_RELOCATABLE
#define CXXBRIDGE1_RELOCATABLE
namespace detail
{
template <typename... Ts> struct make_void
{
  using type = void;
};

template <typename... Ts> using void_t = typename make_void<Ts...>::type;

template <typename Void, template <typename...> class, typename...> struct detect : std::false_type
{
};
template <template <typename...> class T, typename... A>
struct detect<void_t<T<A...> >, T, A...> : std::true_type
{
};

template <template <typename...> class T, typename... A> using is_detected = detect<void, T, A...>;

template <typename T> using detect_IsRelocatable = typename T::IsRelocatable;

template <typename T>
struct get_IsRelocatable : std::is_same<typename T::IsRelocatable, std::true_type>
{
};
} // namespace detail

template <typename T>
struct IsRelocatable
    : std::conditional<
          detail::is_detected<detail::detect_IsRelocatable, T>::value, detail::get_IsRelocatable<T>,
          std::integral_constant<bool, std::is_trivially_move_constructible<T>::value
                                           && std::is_trivially_destructible<T>::value> >::type
{
};
#endif // CXXBRIDGE1_RELOCATABLE

class Str::uninit
{
};
inline Str::Str (uninit) noexcept {}

template <typename T> class Slice<T>::uninit
{
};
template <typename T> inline Slice<T>::Slice (uninit) noexcept {}

namespace repr
{
using Fat = ::std::array< ::std::uintptr_t, 2>;

struct PtrLen final
{
  void *ptr;
  ::std::size_t len;
};
} // namespace repr

namespace detail
{
template <typename T, typename = void *> struct operator_new
{
  void *
  operator() (::std::size_t sz)
  {
    return ::operator new (sz);
  }
};

template <typename T> struct operator_new<T, decltype (T::operator new (sizeof (T)))>
{
  void *
  operator() (::std::size_t sz)
  {
    return T::operator new (sz);
  }
};

class Fail final
{
  ::rust::repr::PtrLen &throw$;

public:
  Fail (::rust::repr::PtrLen &throw$) noexcept : throw$ (throw$) {}
  void operator() (const char *) noexcept;
  void operator() (const std::string &) noexcept;
};
} // namespace detail

template <typename T> union ManuallyDrop
{
  T value;
  ManuallyDrop (T &&value) : value (::std::move (value)) {}
  ~ManuallyDrop () {}
};

template <typename T> union MaybeUninit
{
  T value;
  void *
  operator new (::std::size_t sz)
  {
    return detail::operator_new<T>{}(sz);
  }
  MaybeUninit () {}
  ~MaybeUninit () {}
};

namespace
{
template <> class impl<Str> final
{
public:
  static Str
  new_unchecked (repr::Fat repr) noexcept
  {
    Str str = Str::uninit{};
    str.repr = repr;
    return str;
  }
};

template <typename T> class impl<Slice<T> > final
{
public:
  static Slice<T>
  slice (repr::Fat repr) noexcept
  {
    Slice<T> slice = typename Slice<T>::uninit{};
    slice.repr = repr;
    return slice;
  }
};

template <> class impl<Error> final
{
public:
  static Error
  error (repr::PtrLen repr) noexcept
  {
    Error error;
    error.msg = static_cast<const char *> (repr.ptr);
    error.len = repr.len;
    return error;
  }
};

template <bool> struct deleter_if
{
  template <typename T>
  void
  operator() (T *)
  {
  }
};

template <> struct deleter_if<true>
{
  template <typename T>
  void
  operator() (T *ptr)
  {
    ptr->~T ();
  }
};
} // namespace
} // namespace cxxbridge1

namespace behavior
{
class missing
{
};
missing trycatch (...);

template <typename Try, typename Fail>
static typename ::std::enable_if< ::std::is_same<
    decltype (trycatch (::std::declval<Try> (), ::std::declval<Fail> ())), missing>::value>::type
trycatch (Try &&func, Fail &&fail) noexcept
try
  {
    func ();
  }
catch (const ::std::exception &e)
  {
    fail (e.what ());
  }
} // namespace behavior
} // namespace rust

namespace rpmostreecxx
{
struct StringMapping;
enum class SystemHostType : ::std::uint8_t;
enum class BubblewrapMutability : ::std::uint8_t;
struct Bubblewrap;
struct ContainerImageState;
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
  void run (const ::rpmostreecxx::GCancellable &cancellable);
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

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_rpmostreecxx$ContainerImageState

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
  void tweak_imported_file_info (const ::rpmostreecxx::GFileInfo &file_info) const noexcept;
  bool is_file_filtered (::rust::Str path, const ::rpmostreecxx::GFileInfo &file_info) const;
  void translate_to_tmpfiles_entry (::rust::Str abs_path,
                                    const ::rpmostreecxx::GFileInfo &file_info,
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

  bool operator== (const HistoryEntry &) const noexcept;
  bool operator!= (const HistoryEntry &) const noexcept;
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

  bool operator== (const Refspec &) const noexcept;
  bool operator!= (const Refspec &) const noexcept;
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

  bool operator== (const OverrideReplacement &) const noexcept;
  bool operator!= (const OverrideReplacement &) const noexcept;
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
  ::rust::String get_checksum (const ::rpmostreecxx::OstreeRepo &repo) const;
  ::rust::String get_ostree_ref () const noexcept;
  ::rust::Slice<const ::rpmostreecxx::RepoPackage> get_repo_packages () const noexcept;
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
  generate_treefile (const ::rpmostreecxx::Treefile &src) const;
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
} // namespace rpmostreecxx

static_assert (
    ::rust::IsRelocatable< ::rpmostreecxx::GObject>::value,
    "type rpmostreecxx::GObject should be trivially move constructible and trivially destructible "
    "in C++ to be used as a non-pinned mutable reference in signature of `get` in Rust");
static_assert (::rust::IsRelocatable< ::dnfcxx::FFIDnfPackage>::value,
               "type dnfcxx::FFIDnfPackage should be trivially move constructible and trivially "
               "destructible in C++ to be used as a non-pinned mutable reference in signature of "
               "`get_repodata_chksum_repr` in Rust");

namespace rpmostreecxx
{
extern "C"
{
  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$is_bare_split_xattrs (bool *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$is_http_arg (::rust::Str arg) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$is_ostree_container (bool *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$get_system_host_type (::rpmostreecxx::SystemHostType *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$require_system_host_type (::rpmostreecxx::SystemHostType t) noexcept;

  bool rpmostreecxx$cxxbridge1$is_rpm_arg (::rust::Str arg) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$client_start_daemon () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$client_handle_fd_argument (
      ::rust::Str arg, ::rust::Str arch, bool is_replace,
      ::rust::Vec< ::std::int32_t> *return$) noexcept;

  void
  rpmostreecxx$cxxbridge1$client_render_download_progress (const ::rpmostreecxx::GVariant &progress,
                                                           ::rust::String *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$running_in_container () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$confirm (bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$confirm_or_abort () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Bubblewrap$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Bubblewrap$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$bubblewrap_selftest () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$bubblewrap_run_sync (
      ::std::int32_t rootfs_dfd, const ::rust::Vec< ::rust::String> &args, bool capture_stdout,
      bool unified_core, ::rust::Vec< ::std::uint8_t> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$bubblewrap_new (
      ::std::int32_t rootfs_fd, ::rust::Box< ::rpmostreecxx::Bubblewrap> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$bubblewrap_new_with_mutability (
      ::std::int32_t rootfs_fd, ::rpmostreecxx::BubblewrapMutability mutability,
      ::rust::Box< ::rpmostreecxx::Bubblewrap> *return$) noexcept;

  ::std::int32_t rpmostreecxx$cxxbridge1$Bubblewrap$get_rootfs_fd (
      const ::rpmostreecxx::Bubblewrap &self) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$append_bwrap_arg (::rpmostreecxx::Bubblewrap &self,
                                                            ::rust::Str arg) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$append_child_arg (::rpmostreecxx::Bubblewrap &self,
                                                            ::rust::Str arg) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$setenv (::rpmostreecxx::Bubblewrap &self, ::rust::Str k,
                                                  ::rust::Str v) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$take_fd (::rpmostreecxx::Bubblewrap &self,
                                                   ::std::int32_t source_fd,
                                                   ::std::int32_t target_fd) noexcept;

  void
  rpmostreecxx$cxxbridge1$Bubblewrap$set_inherit_stdin (::rpmostreecxx::Bubblewrap &self) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$take_stdin_fd (::rpmostreecxx::Bubblewrap &self,
                                                         ::std::int32_t source_fd) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$take_stdout_fd (::rpmostreecxx::Bubblewrap &self,
                                                          ::std::int32_t source_fd) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$take_stderr_fd (::rpmostreecxx::Bubblewrap &self,
                                                          ::std::int32_t source_fd) noexcept;

  void
  rpmostreecxx$cxxbridge1$Bubblewrap$take_stdout_and_stderr_fd (::rpmostreecxx::Bubblewrap &self,
                                                                ::std::int32_t source_fd) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$bind_read (::rpmostreecxx::Bubblewrap &self,
                                                     ::rust::Str src, ::rust::Str dest) noexcept;

  void rpmostreecxx$cxxbridge1$Bubblewrap$bind_readwrite (::rpmostreecxx::Bubblewrap &self,
                                                          ::rust::Str src,
                                                          ::rust::Str dest) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Bubblewrap$setup_compat_var (::rpmostreecxx::Bubblewrap &self) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Bubblewrap$run (::rpmostreecxx::Bubblewrap &self,
                                          const ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$applylive_entrypoint (const ::rust::Vec< ::rust::String> &args) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$applylive_finish (const ::rpmostreecxx::OstreeSysroot &sysroot) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$composeutil_legacy_prep_dev_and_run (::std::int32_t rootfs_dfd) noexcept;

  void rpmostreecxx$cxxbridge1$print_ostree_txn_stats (
      ::rpmostreecxx::OstreeRepoTransactionStats &stats) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$write_commit_id (::rust::Str target_path,
                                                                ::rust::Str revision) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$cliwrap_write_wrappers (::std::int32_t rootfs) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$cliwrap_write_some_wrappers (
      ::std::int32_t rootfs, const ::rust::Vec< ::rust::String> &args) noexcept;

  void rpmostreecxx$cxxbridge1$cliwrap_destdir (::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$container_encapsulate (::rust::Vec< ::rust::String> *args) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$deploy_from_self_entrypoint (::rust::Vec< ::rust::String> *args) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$pull_container (
      const ::rpmostreecxx::OstreeRepo &repo, const ::rpmostreecxx::GCancellable &cancellable,
      ::rust::Str imgref, ::rust::Box< ::rpmostreecxx::ContainerImageState> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$container_prune (
      const ::rpmostreecxx::OstreeRepo &repo,
      const ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$query_container_image_commit (
      const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str c,
      ::rust::Box< ::rpmostreecxx::ContainerImageState> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$purge_refspec (const ::rpmostreecxx::OstreeRepo &repo,
                                         ::rust::Str refspec) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TempEtcGuard$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TempEtcGuard$operator$alignof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$FilesystemScriptPrep$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$FilesystemScriptPrep$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$prepare_tempetc_guard (
      ::std::int32_t rootfs, ::rust::Box< ::rpmostreecxx::TempEtcGuard> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$TempEtcGuard$undo (const ::rpmostreecxx::TempEtcGuard &self) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$prepare_filesystem_script_prep (
      ::std::int32_t rootfs, ::rust::Box< ::rpmostreecxx::FilesystemScriptPrep> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$FilesystemScriptPrep$undo (
      ::rpmostreecxx::FilesystemScriptPrep &self) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$run_depmod (::std::int32_t rootfs_dfd,
                                                           ::rust::Str kver,
                                                           bool unified_core) noexcept;

  void rpmostreecxx$cxxbridge1$log_treefile (const ::rpmostreecxx::Treefile &tf) noexcept;

  bool rpmostreecxx$cxxbridge1$is_container_image_reference (::rust::Str refspec) noexcept;

  ::rpmostreecxx::RefspecType
  rpmostreecxx$cxxbridge1$refspec_classify (::rust::Str refspec) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$verify_kernel_hmac (::std::int32_t rootfs,
                                                                   ::rust::Str moddir) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$stage_container_rpms (::rust::Vec< ::rust::String> *rpms,
                                                ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$stage_container_rpm_raw_fds (
      ::rust::Vec< ::std::int32_t> *fds, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$commit_has_matching_sepolicy (
      const ::rpmostreecxx::GVariant &commit, const ::rpmostreecxx::OstreeSePolicy &policy,
      bool *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$get_header_variant (const ::rpmostreecxx::OstreeRepo &repo,
                                              ::rust::Str cachebranch,
                                              ::rpmostreecxx::GVariant **return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$compose_image (::rust::Vec< ::rust::String> *args) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$configure_build_repo_from_target (
      const ::rpmostreecxx::OstreeRepo &build_repo,
      const ::rpmostreecxx::OstreeRepo &target_repo) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$compose_prepare_rootfs (::std::int32_t src_rootfs_dfd,
                                                  ::std::int32_t dest_rootfs_dfd,
                                                  ::rpmostreecxx::Treefile &treefile) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$composepost_nsswitch_altfiles (::std::int32_t rootfs_dfd) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$compose_postprocess (
      ::std::int32_t rootfs_dfd, ::rpmostreecxx::Treefile &treefile, ::rust::Str next_version,
      bool unified_core) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$compose_postprocess_final (::std::int32_t rootfs_dfd) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$convert_var_to_tmpfiles_d (
      ::std::int32_t rootfs_dfd, const ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$rootfs_prepare_links (::std::int32_t rootfs_dfd) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$workaround_selinux_cross_labeling (
      ::std::int32_t rootfs_dfd, ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$prepare_rpmdb_base_location (
      ::std::int32_t rootfs_dfd, ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$compose_postprocess_rpm_macro (::std::int32_t rootfs_dfd) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$postprocess_cleanup_rpmdb (::std::int32_t rootfs_dfd) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$rewrite_rpmdb_for_target (::std::int32_t rootfs_dfd,
                                                                         bool normalize) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$directory_size (::std::int32_t dfd,
                                          const ::rpmostreecxx::GCancellable &cancellable,
                                          ::std::uint64_t *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$container_rebuild (::rust::Str treefile) noexcept
  {
    void (*container_rebuild$) (::rust::Str) = ::rpmostreecxx::container_rebuild;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          container_rebuild$ (treefile);
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$deployment_for_id (::rpmostreecxx::OstreeSysroot &sysroot,
                                             ::rust::Str deploy_id,
                                             ::rpmostreecxx::OstreeDeployment **return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$deployment_checksum_for_id (::rpmostreecxx::OstreeSysroot &sysroot,
                                                      ::rust::Str deploy_id,
                                                      ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$deployment_get_base (::rpmostreecxx::OstreeSysroot &sysroot,
                                               ::rust::Str opt_deploy_id, ::rust::Str opt_os_name,
                                               ::rpmostreecxx::OstreeDeployment **return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$daemon_main (bool debug) noexcept;

  void rpmostreecxx$cxxbridge1$daemon_terminate () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$daemon_sanitycheck_environment (
      const ::rpmostreecxx::OstreeSysroot &sysroot) noexcept;

  void rpmostreecxx$cxxbridge1$deployment_generate_id (
      const ::rpmostreecxx::OstreeDeployment &deployment, ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$deployment_populate_variant (
      const ::rpmostreecxx::OstreeSysroot &sysroot,
      const ::rpmostreecxx::OstreeDeployment &deployment,
      const ::rpmostreecxx::GVariantDict &dict) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$generate_baselayer_refs (
      const ::rpmostreecxx::OstreeSysroot &sysroot, const ::rpmostreecxx::OstreeRepo &repo,
      const ::rpmostreecxx::GCancellable &cancellable) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$variant_add_remote_status (
      const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str refspec, ::rust::Str base_checksum,
      const ::rpmostreecxx::GVariantDict &dict) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$deployment_layeredmeta_from_commit (
      const ::rpmostreecxx::OstreeDeployment &deployment, const ::rpmostreecxx::GVariant &commit,
      ::rpmostreecxx::DeploymentLayeredMeta *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$deployment_layeredmeta_load (
      const ::rpmostreecxx::OstreeRepo &repo, const ::rpmostreecxx::OstreeDeployment &deployment,
      ::rpmostreecxx::DeploymentLayeredMeta *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$parse_override_source (
      ::rust::Str source, ::rpmostreecxx::OverrideReplacementSource *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$parse_revision (::rust::Str source,
                                          ::rpmostreecxx::ParsedRevision *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$generate_object_path (::rust::Str base, ::rust::Str next_segment,
                                                ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$failpoint (::rust::Str p) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RpmImporterFlags$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RpmImporterFlags$operator$alignof () noexcept;

  ::rpmostreecxx::RpmImporterFlags *
  rpmostreecxx$cxxbridge1$rpm_importer_flags_new_empty () noexcept;

  bool rpmostreecxx$cxxbridge1$RpmImporterFlags$is_ima_enabled (
      const ::rpmostreecxx::RpmImporterFlags &self) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RpmImporter$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RpmImporter$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$rpm_importer_new (
      ::rust::Str pkg_name, ::rust::Str ostree_branch,
      const ::rpmostreecxx::RpmImporterFlags &flags,
      ::rust::Box< ::rpmostreecxx::RpmImporter> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$handle_translate_pathname (
      ::rpmostreecxx::RpmImporter &self, ::rust::Str path, ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$ostree_branch (const ::rpmostreecxx::RpmImporter &self,
                                                          ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$pkg_name (const ::rpmostreecxx::RpmImporter &self,
                                                     ::rust::String *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$RpmImporter$doc_files_are_filtered (
      const ::rpmostreecxx::RpmImporter &self) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$doc_files_insert (::rpmostreecxx::RpmImporter &self,
                                                             ::rust::Str path) noexcept;

  bool
  rpmostreecxx$cxxbridge1$RpmImporter$doc_files_contains (const ::rpmostreecxx::RpmImporter &self,
                                                          ::rust::Str path) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_insert (
      ::rpmostreecxx::RpmImporter &self, ::rust::Str path, ::std::uint64_t index) noexcept;

  bool rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_contains (
      const ::rpmostreecxx::RpmImporter &self, ::rust::Str path) noexcept;

  ::std::uint64_t
  rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_get (const ::rpmostreecxx::RpmImporter &self,
                                                           ::rust::Str path) noexcept;

  bool rpmostreecxx$cxxbridge1$RpmImporter$is_ima_enabled (
      const ::rpmostreecxx::RpmImporter &self) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$tweak_imported_file_info (
      const ::rpmostreecxx::RpmImporter &self, const ::rpmostreecxx::GFileInfo &file_info) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$RpmImporter$is_file_filtered (
      const ::rpmostreecxx::RpmImporter &self, ::rust::Str path,
      const ::rpmostreecxx::GFileInfo &file_info, bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$RpmImporter$translate_to_tmpfiles_entry (
      ::rpmostreecxx::RpmImporter &self, ::rust::Str abs_path,
      const ::rpmostreecxx::GFileInfo &file_info, ::rust::Str username,
      ::rust::Str groupname) noexcept;

  bool rpmostreecxx$cxxbridge1$RpmImporter$has_tmpfiles_entries (
      const ::rpmostreecxx::RpmImporter &self) noexcept;

  void rpmostreecxx$cxxbridge1$RpmImporter$serialize_tmpfiles_content (
      const ::rpmostreecxx::RpmImporter &self, ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$tmpfiles_translate (
      ::rust::Str abs_path, const ::rpmostreecxx::GFileInfo &file_info, ::rust::Str username,
      ::rust::Str groupname, ::rust::String *return$) noexcept;

  ::rust::repr::Fat rpmostreecxx$cxxbridge1$get_dracut_random_cpio () noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$initramfs_overlay_generate (const ::rust::Vec< ::rust::String> &files,
                                                      ::rpmostreecxx::GCancellable &cancellable,
                                                      ::std::int32_t *return$) noexcept;

  void rpmostreecxx$cxxbridge1$journal_print_staging_failure () noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_begin_task (::rust::Str msg) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_begin_n_items (::rust::Str msg,
                                                               ::std::uint64_t n) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_begin_percent (::rust::Str msg) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_set_message (::rust::Str msg) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_set_sub_message (::rust::Str msg) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_update (::std::uint64_t n) noexcept;

  void rpmostreecxx$cxxbridge1$console_progress_end (::rust::Str suffix) noexcept;
  bool rpmostreecxx$cxxbridge1$HistoryEntry$operator$eq (const HistoryEntry &,
                                                         const HistoryEntry &) noexcept;
  bool rpmostreecxx$cxxbridge1$HistoryEntry$operator$ne (const HistoryEntry &,
                                                         const HistoryEntry &) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$HistoryCtx$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$HistoryCtx$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$history_ctx_new (
      ::rust::Box< ::rpmostreecxx::HistoryCtx> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$HistoryCtx$next_entry (::rpmostreecxx::HistoryCtx &self,
                                                 ::rpmostreecxx::HistoryEntry *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$history_prune () noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$modularity_entrypoint (const ::rust::Vec< ::rust::String> &args) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TokioHandle$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TokioHandle$operator$alignof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TokioEnterGuard$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$TokioEnterGuard$operator$alignof () noexcept;

  ::rpmostreecxx::TokioHandle *rpmostreecxx$cxxbridge1$tokio_handle_get () noexcept;

  ::rpmostreecxx::TokioEnterGuard *
  rpmostreecxx$cxxbridge1$TokioHandle$enter (const ::rpmostreecxx::TokioHandle &self) noexcept;

  bool rpmostreecxx$cxxbridge1$script_is_ignored (::rust::Str pkg, ::rust::Str script) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$testutils_entrypoint (::rust::Vec< ::rust::String> *argv) noexcept;

  void rpmostreecxx$cxxbridge1$maybe_shell_quote (::rust::Str input,
                                                  ::rust::String *return$) noexcept;
  bool rpmostreecxx$cxxbridge1$Refspec$operator$eq (const Refspec &, const Refspec &) noexcept;
  bool
  rpmostreecxx$cxxbridge1$OverrideReplacement$operator$eq (const OverrideReplacement &,
                                                           const OverrideReplacement &) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Treefile$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Treefile$operator$alignof () noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$treefile_new (::rust::Str filename, ::rust::Str basearch,
                                        ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$treefile_new_empty (
      ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$treefile_new_from_string (
      ::rust::Str buf, bool client, ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$treefile_new_compose (
      ::rust::Str filename, ::rust::Str basearch,
      ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$treefile_new_client (
      ::rust::Str filename, ::rust::Str basearch,
      ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$treefile_new_client_from_etc (
      ::rust::Str basearch, ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$treefile_delete_client_etc (::std::uint32_t *return$) noexcept;

  ::rust::repr::Fat
  rpmostreecxx$cxxbridge1$Treefile$get_workdir (const ::rpmostreecxx::Treefile &self) noexcept;

  ::std::int32_t
  rpmostreecxx$cxxbridge1$Treefile$get_passwd_fd (::rpmostreecxx::Treefile &self) noexcept;

  ::std::int32_t
  rpmostreecxx$cxxbridge1$Treefile$get_group_fd (::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_json_string (const ::rpmostreecxx::Treefile &self,
                                                         ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_ostree_layers (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_ostree_override_layers (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_all_ostree_layers (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_repos (const ::rpmostreecxx::Treefile &self,
                                                   ::rust::Vec< ::rust::String> *return$) noexcept;

  void
  rpmostreecxx$cxxbridge1$Treefile$get_packages (const ::rpmostreecxx::Treefile &self,
                                                 ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$require_automatic_version_prefix (
      const ::rpmostreecxx::Treefile &self, ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$add_packages (::rpmostreecxx::Treefile &self,
                                                 ::rust::Vec< ::rust::String> *packages,
                                                 bool allow_existing, bool *return$) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$has_packages (const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_local_packages (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$add_local_packages (::rpmostreecxx::Treefile &self,
                                                       ::rust::Vec< ::rust::String> *packages,
                                                       bool allow_existing, bool *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_local_fileoverride_packages (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$add_local_fileoverride_packages (
      ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *packages, bool allow_existing,
      bool *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$remove_packages (::rpmostreecxx::Treefile &self,
                                                    ::rust::Vec< ::rust::String> *packages,
                                                    bool allow_noent, bool *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_packages_override_replace (
      const ::rpmostreecxx::Treefile &self,
      ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$has_packages_override_replace (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$add_packages_override_replace (
      ::rpmostreecxx::Treefile &self, ::rpmostreecxx::OverrideReplacement *replacement) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$remove_package_override_replace (::rpmostreecxx::Treefile &self,
                                                                    ::rust::Str package) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_packages_override_replace_local (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$add_packages_override_replace_local (
      ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *packages) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$remove_package_override_replace_local (
      ::rpmostreecxx::Treefile &self, ::rust::Str package) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_packages_override_remove (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$add_packages_override_remove (
      ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *packages) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$remove_package_override_remove (::rpmostreecxx::Treefile &self,
                                                                   ::rust::Str package) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$has_packages_override_remove_name (
      const ::rpmostreecxx::Treefile &self, ::rust::Str name) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$remove_all_overrides (::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_modules_enable (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$has_modules_enable (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_modules_install (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$add_modules (::rpmostreecxx::Treefile &self,
                                                     ::rust::Vec< ::rust::String> *modules,
                                                     bool enable_only) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$remove_modules (::rpmostreecxx::Treefile &self,
                                                        ::rust::Vec< ::rust::String> *modules,
                                                        bool enable_only) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$remove_all_packages (::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_exclude_packages (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_platform_module (const ::rpmostreecxx::Treefile &self,
                                                             ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_install_langs (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void
  rpmostreecxx$cxxbridge1$Treefile$format_install_langs_macro (const ::rpmostreecxx::Treefile &self,
                                                               ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_lockfile_repos (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::Fat
  rpmostreecxx$cxxbridge1$Treefile$get_ref (const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_cliwrap (const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_cliwrap_binaries (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$set_cliwrap (::rpmostreecxx::Treefile &self,
                                                     bool enabled) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_container_cmd (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_readonly_executables (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_documentation (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$get_recommends (const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_selinux (const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_gpg_key (const ::rpmostreecxx::Treefile &self,
                                                     ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_automatic_version_suffix (
      const ::rpmostreecxx::Treefile &self, ::rust::String *return$) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$get_container (const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_machineid_compat (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_etc_group_members (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_boot_location_is_modules (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_ima (const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_releasever (const ::rpmostreecxx::Treefile &self,
                                                        ::rust::String *return$) noexcept;

  ::rpmostreecxx::RepoMetadataTarget rpmostreecxx$cxxbridge1$Treefile$get_repo_metadata_target (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$rpmdb_backend_is_target (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$should_normalize_rpmdb (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_files_remove_regex (
      const ::rpmostreecxx::Treefile &self, ::rust::Str package,
      ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$get_checksum (const ::rpmostreecxx::Treefile &self,
                                                 const ::rpmostreecxx::OstreeRepo &repo,
                                                 ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_ostree_ref (const ::rpmostreecxx::Treefile &self,
                                                        ::rust::String *return$) noexcept;

  ::rust::repr::Fat rpmostreecxx$cxxbridge1$Treefile$get_repo_packages (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void
  rpmostreecxx$cxxbridge1$Treefile$clear_repo_packages (::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$prettyprint_json_stdout (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$print_deprecation_warnings (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$print_experimental_notices (
      const ::rpmostreecxx::Treefile &self) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$sanitycheck_externals (
      const ::rpmostreecxx::Treefile &self) noexcept;

  ::rpmostreecxx::RpmImporterFlags *
  rpmostreecxx$cxxbridge1$Treefile$importer_flags (const ::rpmostreecxx::Treefile &self,
                                                   ::rust::Str pkg_name) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$write_repovars (const ::rpmostreecxx::Treefile &self,
                                                   ::std::int32_t workdir_dfd_raw,
                                                   ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$set_releasever (::rpmostreecxx::Treefile &self,
                                                   ::rust::Str releasever) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$enable_repo (::rpmostreecxx::Treefile &self,
                                                                     ::rust::Str repo) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$disable_repo (::rpmostreecxx::Treefile &self,
                                                 ::rust::Str repo) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Treefile$validate_for_container (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void
  rpmostreecxx$cxxbridge1$Treefile$get_base_refspec (const ::rpmostreecxx::Treefile &self,
                                                     ::rpmostreecxx::Refspec *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$rebase (::rpmostreecxx::Treefile &self,
                                                ::rust::Str new_refspec,
                                                ::rust::Str custom_origin_url,
                                                ::rust::Str custom_origin_description) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_origin_custom_url (const ::rpmostreecxx::Treefile &self,
                                                               ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_origin_custom_description (
      const ::rpmostreecxx::Treefile &self, ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_override_commit (const ::rpmostreecxx::Treefile &self,
                                                             ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$set_override_commit (::rpmostreecxx::Treefile &self,
                                                             ::rust::Str checksum) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_initramfs_etc_files (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$has_initramfs_etc_files (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_track (
      ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *files) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_untrack (
      ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *files) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_untrack_all (
      ::rpmostreecxx::Treefile &self) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$get_initramfs_regenerate (
      const ::rpmostreecxx::Treefile &self) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$get_initramfs_args (
      const ::rpmostreecxx::Treefile &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Treefile$set_initramfs_regenerate (
      ::rpmostreecxx::Treefile &self, bool enabled, ::rust::Vec< ::rust::String> *args) noexcept;

  void
  rpmostreecxx$cxxbridge1$Treefile$get_unconfigured_state (const ::rpmostreecxx::Treefile &self,
                                                           ::rust::String *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$Treefile$may_require_local_assembly (
      const ::rpmostreecxx::Treefile &self) noexcept;

  bool
  rpmostreecxx$cxxbridge1$Treefile$has_any_packages (const ::rpmostreecxx::Treefile &self) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Treefile$merge_treefile (::rpmostreecxx::Treefile &self,
                                                   ::rust::Str treefile, bool *return$) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RepoPackage$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$RepoPackage$operator$alignof () noexcept;

  ::rust::repr::Fat
  rpmostreecxx$cxxbridge1$RepoPackage$get_repo (const ::rpmostreecxx::RepoPackage &self) noexcept;

  void
  rpmostreecxx$cxxbridge1$RepoPackage$get_packages (const ::rpmostreecxx::RepoPackage &self,
                                                    ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$varsubstitute (::rust::Str s,
                                         const ::rust::Vec< ::rpmostreecxx::StringMapping> &vars,
                                         ::rust::String *return$) noexcept;

  void rpmostreecxx$cxxbridge1$get_features (::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$get_rpm_basearch (::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$sealed_memfd (::rust::Str description,
                                        ::rust::Slice<const ::std::uint8_t> content,
                                        ::std::int32_t *return$) noexcept;

  bool rpmostreecxx$cxxbridge1$running_in_systemd () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$calculate_advisories_diff (
      const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str checksum_from, ::rust::Str checksum_to,
      ::rpmostreecxx::GVariant **return$) noexcept;

  void rpmostreecxx$cxxbridge1$translate_path_for_ostree (::rust::Str path,
                                                          ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$get_live_apply_state (const ::rpmostreecxx::OstreeSysroot &sysroot,
                                                const ::rpmostreecxx::OstreeDeployment &deployment,
                                                ::rpmostreecxx::LiveApplyState *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$has_live_apply_state (const ::rpmostreecxx::OstreeSysroot &sysroot,
                                                const ::rpmostreecxx::OstreeDeployment &deployment,
                                                bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$applylive_sync_ref (
      const ::rpmostreecxx::OstreeSysroot &sysroot) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$transaction_apply_live (const ::rpmostreecxx::OstreeSysroot &sysroot,
                                                  const ::rpmostreecxx::GVariant &target) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$prepare_rpm_layering (::std::int32_t rootfs,
                                                                     ::rust::Str merge_passwd_dir,
                                                                     bool *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$complete_rpm_layering (::std::int32_t rootfs) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$passwd_cleanup (::std::int32_t rootfs) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$migrate_group_except_root (
      ::std::int32_t rootfs, const ::rust::Vec< ::rust::String> &preserved_groups) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$migrate_passwd_except_root (::std::int32_t rootfs) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$passwd_compose_prep (::std::int32_t rootfs,
                                               ::rpmostreecxx::Treefile &treefile) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$passwd_compose_prep_repo (
      ::std::int32_t rootfs, ::rpmostreecxx::Treefile &treefile,
      const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str previous_checksum,
      bool unified_core) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$dir_contains_uid (::std::int32_t dirfd,
                                                                 ::std::uint32_t id,
                                                                 bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$dir_contains_gid (::std::int32_t dirfd,
                                                                 ::std::uint32_t id,
                                                                 bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$check_passwd_group_entries (
      const ::rpmostreecxx::OstreeRepo &ffi_repo, ::std::int32_t rootfs_dfd,
      ::rpmostreecxx::Treefile &treefile, ::rust::Str previous_rev) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$passwddb_open (::std::int32_t rootfs,
                                         ::rust::Box< ::rpmostreecxx::PasswdDB> *return$) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$PasswdDB$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$PasswdDB$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$PasswdDB$lookup_user (
      const ::rpmostreecxx::PasswdDB &self, ::std::uint32_t uid, ::rust::String *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$PasswdDB$lookup_group (
      const ::rpmostreecxx::PasswdDB &self, ::std::uint32_t gid, ::rust::String *return$) noexcept;

  ::rpmostreecxx::PasswdEntries *rpmostreecxx$cxxbridge1$new_passwd_entries () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$PasswdEntries$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$PasswdEntries$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$PasswdEntries$add_group_content (
      ::rpmostreecxx::PasswdEntries &self, ::std::int32_t rootfs, ::rust::Str path) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$PasswdEntries$add_passwd_content (
      ::rpmostreecxx::PasswdEntries &self, ::std::int32_t rootfs, ::rust::Str path) noexcept;

  bool
  rpmostreecxx$cxxbridge1$PasswdEntries$contains_group (const ::rpmostreecxx::PasswdEntries &self,
                                                        ::rust::Str user) noexcept;

  bool
  rpmostreecxx$cxxbridge1$PasswdEntries$contains_user (const ::rpmostreecxx::PasswdEntries &self,
                                                       ::rust::Str user) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$PasswdEntries$lookup_user_id (const ::rpmostreecxx::PasswdEntries &self,
                                                        ::rust::Str user,
                                                        ::std::uint32_t *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$PasswdEntries$lookup_group_id (const ::rpmostreecxx::PasswdEntries &self,
                                                         ::rust::Str group,
                                                         ::std::uint32_t *return$) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Extensions$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$Extensions$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$extensions_load (
      ::rust::Str path, ::rust::Str basearch,
      const ::rust::Vec< ::rpmostreecxx::StringMapping> &base_pkgs,
      ::rust::Box< ::rpmostreecxx::Extensions> *return$) noexcept;

  void
  rpmostreecxx$cxxbridge1$Extensions$get_repos (const ::rpmostreecxx::Extensions &self,
                                                ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Extensions$get_os_extension_packages (
      const ::rpmostreecxx::Extensions &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  void rpmostreecxx$cxxbridge1$Extensions$get_development_packages (
      const ::rpmostreecxx::Extensions &self, ::rust::Vec< ::rust::String> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Extensions$state_checksum_changed (
      const ::rpmostreecxx::Extensions &self, ::rust::Str chksum, ::rust::Str output_dir,
      bool *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Extensions$update_state_checksum (
      const ::rpmostreecxx::Extensions &self, ::rust::Str chksum, ::rust::Str output_dir) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$Extensions$serialize_to_dir (const ::rpmostreecxx::Extensions &self,
                                                       ::rust::Str output_dir) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$Extensions$generate_treefile (
      const ::rpmostreecxx::Extensions &self, const ::rpmostreecxx::Treefile &src,
      ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$LockfileConfig$operator$sizeof () noexcept;
  ::std::size_t rpmostreecxx$cxxbridge1$LockfileConfig$operator$alignof () noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$lockfile_read (
      const ::rust::Vec< ::rust::String> &filenames,
      ::rust::Box< ::rpmostreecxx::LockfileConfig> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$lockfile_write (::rust::Str filename,
                                          ::rpmostreecxx::CxxGObjectArray &packages,
                                          ::rpmostreecxx::CxxGObjectArray &rpmmd_repos) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$LockfileConfig$get_locked_packages (
      const ::rpmostreecxx::LockfileConfig &self,
      ::rust::Vec< ::rpmostreecxx::LockedPackage> *return$) noexcept;

  ::rust::repr::PtrLen rpmostreecxx$cxxbridge1$origin_to_treefile (
      const ::rpmostreecxx::GKeyFile &kf, ::rust::Box< ::rpmostreecxx::Treefile> *return$) noexcept;

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$treefile_to_origin (const ::rpmostreecxx::Treefile &tf,
                                              ::rpmostreecxx::GKeyFile **return$) noexcept;

  void
  rpmostreecxx$cxxbridge1$origin_validate_roundtrip (const ::rpmostreecxx::GKeyFile &kf) noexcept;

  void rpmostreecxx$cxxbridge1$cache_branch_to_nevra (::rust::Str nevra,
                                                      ::rust::String *return$) noexcept;

  ::std::uint32_t
  rpmostreecxx$cxxbridge1$CxxGObjectArray$length (::rpmostreecxx::CxxGObjectArray &self) noexcept
  {
    ::std::uint32_t (::rpmostreecxx::CxxGObjectArray::*length$) ()
        = &::rpmostreecxx::CxxGObjectArray::length;
    return (self.*length$) ();
  }

  ::rpmostreecxx::GObject *
  rpmostreecxx$cxxbridge1$CxxGObjectArray$get (::rpmostreecxx::CxxGObjectArray &self,
                                               ::std::uint32_t i) noexcept
  {
    ::rpmostreecxx::GObject &(::rpmostreecxx::CxxGObjectArray::*get$) (::std::uint32_t)
        = &::rpmostreecxx::CxxGObjectArray::get;
    return &(self.*get$) (i);
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$util_next_version (::rust::Str auto_version_prefix,
                                             ::rust::Str version_suffix, ::rust::Str last_version,
                                             ::rust::String *return$) noexcept
  {
    ::rust::String (*util_next_version$) (::rust::Str, ::rust::Str, ::rust::Str)
        = ::rpmostreecxx::util_next_version;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rust::String (
              util_next_version$ (auto_version_prefix, version_suffix, last_version));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::std::int32_t
  rpmostreecxx$cxxbridge1$testutil_validate_cxxrs_passthrough (
      const ::rpmostreecxx::OstreeRepo &repo) noexcept
  {
    ::std::int32_t (*testutil_validate_cxxrs_passthrough$) (const ::rpmostreecxx::OstreeRepo &)
        = ::rpmostreecxx::testutil_validate_cxxrs_passthrough;
    return testutil_validate_cxxrs_passthrough$ (repo);
  }

  void
  rpmostreecxx$cxxbridge1$early_main () noexcept
  {
    void (*early_main$) () = ::rpmostreecxx::early_main;
    early_main$ ();
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$rpmostree_main (::rust::Slice<const ::rust::Str> args,
                                          ::std::int32_t *return$) noexcept
  {
    ::std::int32_t (*rpmostree_main$) (::rust::Slice<const ::rust::Str>)
        = ::rpmostreecxx::rpmostree_main;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::std::int32_t (rpmostree_main$ (args));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  void
  rpmostreecxx$cxxbridge1$rpmostree_process_global_teardown () noexcept
  {
    void (*rpmostree_process_global_teardown$) ()
        = ::rpmostreecxx::rpmostree_process_global_teardown;
    rpmostree_process_global_teardown$ ();
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$c_unit_tests () noexcept
  {
    void (*c_unit_tests$) () = ::rpmostreecxx::c_unit_tests;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          c_unit_tests$ ();
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$daemon_init_inner (bool debug) noexcept
  {
    void (*daemon_init_inner$) (bool) = ::rpmostreecxx::daemon_init_inner;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          daemon_init_inner$ (debug);
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$daemon_main_inner () noexcept
  {
    void (*daemon_main_inner$) () = ::rpmostreecxx::daemon_main_inner;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          daemon_main_inner$ ();
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$client_require_root () noexcept
  {
    void (*client_require_root$) () = ::rpmostreecxx::client_require_root;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          client_require_root$ ();
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$new_client_connection (
      ::rpmostreecxx::ClientConnection **return$) noexcept
  {
    ::std::unique_ptr< ::rpmostreecxx::ClientConnection> (*new_client_connection$) ()
        = ::rpmostreecxx::new_client_connection;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rpmostreecxx::ClientConnection *(new_client_connection$ ().release ());
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  const ::rpmostreecxx::GDBusConnection *
  rpmostreecxx$cxxbridge1$ClientConnection$get_connection (
      ::rpmostreecxx::ClientConnection &self) noexcept
  {
    const ::rpmostreecxx::GDBusConnection &(::rpmostreecxx::ClientConnection::*get_connection$) ()
        = &::rpmostreecxx::ClientConnection::get_connection;
    return &(self.*get_connection$) ();
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$ClientConnection$transaction_connect_progress_sync (
      const ::rpmostreecxx::ClientConnection &self, ::rust::Str address) noexcept
  {
    void (::rpmostreecxx::ClientConnection::*transaction_connect_progress_sync$) (::rust::Str) const
        = &::rpmostreecxx::ClientConnection::transaction_connect_progress_sync;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          (self.*transaction_connect_progress_sync$) (address);
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::std::int32_t
  rpmostreecxx$cxxbridge1$RPMDiff$n_removed (const ::rpmostreecxx::RPMDiff &self) noexcept
  {
    ::std::int32_t (::rpmostreecxx::RPMDiff::*n_removed$) () const
        = &::rpmostreecxx::RPMDiff::n_removed;
    return (self.*n_removed$) ();
  }

  ::std::int32_t
  rpmostreecxx$cxxbridge1$RPMDiff$n_added (const ::rpmostreecxx::RPMDiff &self) noexcept
  {
    ::std::int32_t (::rpmostreecxx::RPMDiff::*n_added$) () const
        = &::rpmostreecxx::RPMDiff::n_added;
    return (self.*n_added$) ();
  }

  ::std::int32_t
  rpmostreecxx$cxxbridge1$RPMDiff$n_modified (const ::rpmostreecxx::RPMDiff &self) noexcept
  {
    ::std::int32_t (::rpmostreecxx::RPMDiff::*n_modified$) () const
        = &::rpmostreecxx::RPMDiff::n_modified;
    return (self.*n_modified$) ();
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$rpmdb_diff (const ::rpmostreecxx::OstreeRepo &repo,
                                      const ::std::string &src, const ::std::string &dest,
                                      bool allow_noent, ::rpmostreecxx::RPMDiff **return$) noexcept
  {
    ::std::unique_ptr< ::rpmostreecxx::RPMDiff> (*rpmdb_diff$) (
        const ::rpmostreecxx::OstreeRepo &, const ::std::string &, const ::std::string &, bool)
        = ::rpmostreecxx::rpmdb_diff;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rpmostreecxx::RPMDiff *(
              rpmdb_diff$ (repo, src, dest, allow_noent).release ());
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  void
  rpmostreecxx$cxxbridge1$RPMDiff$print (const ::rpmostreecxx::RPMDiff &self) noexcept
  {
    void (::rpmostreecxx::RPMDiff::*print$) () const = &::rpmostreecxx::RPMDiff::print;
    (self.*print$) ();
  }

  void
  rpmostreecxx$cxxbridge1$print_treepkg_diff_from_sysroot_path (
      ::rust::Str sysroot_path, ::rpmostreecxx::RpmOstreeDiffPrintFormat format,
      ::std::uint32_t max_key_len, ::rpmostreecxx::GCancellable *cancellable) noexcept
  {
    void (*print_treepkg_diff_from_sysroot_path$) (::rust::Str,
                                                   ::rpmostreecxx::RpmOstreeDiffPrintFormat,
                                                   ::std::uint32_t, ::rpmostreecxx::GCancellable *)
        = ::rpmostreecxx::print_treepkg_diff_from_sysroot_path;
    print_treepkg_diff_from_sysroot_path$ (sysroot_path, format, max_key_len, cancellable);
  }

  ::rpmostreecxx::Progress *
  rpmostreecxx$cxxbridge1$progress_begin_task (::rust::Str msg) noexcept
  {
    ::std::unique_ptr< ::rpmostreecxx::Progress> (*progress_begin_task$) (::rust::Str)
        = ::rpmostreecxx::progress_begin_task;
    return progress_begin_task$ (msg).release ();
  }

  void
  rpmostreecxx$cxxbridge1$Progress$end (::rpmostreecxx::Progress &self, ::rust::Str msg) noexcept
  {
    void (::rpmostreecxx::Progress::*end$) (::rust::Str) = &::rpmostreecxx::Progress::end;
    (self.*end$) (msg);
  }

  void
  rpmostreecxx$cxxbridge1$output_message (::rust::Str msg) noexcept
  {
    void (*output_message$) (::rust::Str) = ::rpmostreecxx::output_message;
    output_message$ (msg);
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$nevra_to_cache_branch (const ::std::string &nevra,
                                                 ::rust::String *return$) noexcept
  {
    ::rust::String (*nevra_to_cache_branch$) (const ::std::string &)
        = ::rpmostreecxx::nevra_to_cache_branch;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rust::String (nevra_to_cache_branch$ (nevra));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$get_repodata_chksum_repr (::dnfcxx::FFIDnfPackage &pkg,
                                                    ::rust::String *return$) noexcept
  {
    ::rust::String (*get_repodata_chksum_repr$) (::dnfcxx::FFIDnfPackage &)
        = ::rpmostreecxx::get_repodata_chksum_repr;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rust::String (get_repodata_chksum_repr$ (pkg));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$rpmts_for_commit (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str rev,
                                            ::rpmostreecxx::RpmTs **return$) noexcept
  {
    ::std::unique_ptr< ::rpmostreecxx::RpmTs> (*rpmts_for_commit$) (
        const ::rpmostreecxx::OstreeRepo &, ::rust::Str)
        = ::rpmostreecxx::rpmts_for_commit;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rpmostreecxx::RpmTs *(rpmts_for_commit$ (repo, rev).release ());
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$rpmdb_package_name_list (::std::int32_t dfd, const ::rust::String *path,
                                                   ::rust::Vec< ::rust::String> *return$) noexcept
  {
    ::rust::Vec< ::rust::String> (*rpmdb_package_name_list$) (::std::int32_t, ::rust::String)
        = ::rpmostreecxx::rpmdb_package_name_list;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rust::Vec< ::rust::String> (
              rpmdb_package_name_list$ (dfd, ::rust::String (::rust::unsafe_bitcopy, *path)));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$RpmTs$packages_providing_file (
      const ::rpmostreecxx::RpmTs &self, ::rust::Str path,
      ::rust::Vec< ::rust::String> *return$) noexcept
  {
    ::rust::Vec< ::rust::String> (::rpmostreecxx::RpmTs::*packages_providing_file$) (::rust::Str)
        const
        = &::rpmostreecxx::RpmTs::packages_providing_file;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rust::Vec< ::rust::String> ((self.*packages_providing_file$) (path));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$RpmTs$package_meta (const ::rpmostreecxx::RpmTs &self, ::rust::Str name,
                                              ::rpmostreecxx::PackageMeta **return$) noexcept
  {
    ::std::unique_ptr< ::rpmostreecxx::PackageMeta> (::rpmostreecxx::RpmTs::*package_meta$) (
        ::rust::Str) const
        = &::rpmostreecxx::RpmTs::package_meta;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rpmostreecxx::PackageMeta *((self.*package_meta$) (name).release ());
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }

  ::std::uint64_t
  rpmostreecxx$cxxbridge1$PackageMeta$size (const ::rpmostreecxx::PackageMeta &self) noexcept
  {
    ::std::uint64_t (::rpmostreecxx::PackageMeta::*size$) () const
        = &::rpmostreecxx::PackageMeta::size;
    return (self.*size$) ();
  }

  ::std::uint64_t
  rpmostreecxx$cxxbridge1$PackageMeta$buildtime (const ::rpmostreecxx::PackageMeta &self) noexcept
  {
    ::std::uint64_t (::rpmostreecxx::PackageMeta::*buildtime$) () const
        = &::rpmostreecxx::PackageMeta::buildtime;
    return (self.*buildtime$) ();
  }

  void
  rpmostreecxx$cxxbridge1$PackageMeta$changelogs (const ::rpmostreecxx::PackageMeta &self,
                                                  ::rust::Vec< ::std::uint64_t> *return$) noexcept
  {
    ::rust::Vec< ::std::uint64_t> (::rpmostreecxx::PackageMeta::*changelogs$) () const
        = &::rpmostreecxx::PackageMeta::changelogs;
    new (return$)::rust::Vec< ::std::uint64_t> ((self.*changelogs$) ());
  }

  const ::std::string *
  rpmostreecxx$cxxbridge1$PackageMeta$src_pkg (const ::rpmostreecxx::PackageMeta &self) noexcept
  {
    const ::std::string &(::rpmostreecxx::PackageMeta::*src_pkg$) () const
        = &::rpmostreecxx::PackageMeta::src_pkg;
    return &(self.*src_pkg$) ();
  }

  ::rust::repr::PtrLen
  rpmostreecxx$cxxbridge1$package_variant_list_for_commit (
      const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str rev,
      const ::rpmostreecxx::GCancellable &cancellable, ::rpmostreecxx::GVariant **return$) noexcept
  {
    ::rpmostreecxx::GVariant *(*package_variant_list_for_commit$) (
        const ::rpmostreecxx::OstreeRepo &, ::rust::Str, const ::rpmostreecxx::GCancellable &)
        = ::rpmostreecxx::package_variant_list_for_commit;
    ::rust::repr::PtrLen throw$;
    ::rust::behavior::trycatch (
        [&] {
          new (return$)::rpmostreecxx::GVariant *(
              package_variant_list_for_commit$ (repo, rev, cancellable));
          throw$.ptr = nullptr;
        },
        ::rust::detail::Fail (throw$));
    return throw$;
  }
} // extern "C"

bool
is_bare_split_xattrs ()
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$is_bare_split_xattrs (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
is_http_arg (::rust::Str arg) noexcept
{
  return rpmostreecxx$cxxbridge1$is_http_arg (arg);
}

bool
is_ostree_container ()
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$is_ostree_container (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::SystemHostType
get_system_host_type ()
{
  ::rust::MaybeUninit< ::rpmostreecxx::SystemHostType> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$get_system_host_type (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
require_system_host_type (::rpmostreecxx::SystemHostType t)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$require_system_host_type (t);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
is_rpm_arg (::rust::Str arg) noexcept
{
  return rpmostreecxx$cxxbridge1$is_rpm_arg (arg);
}

void
client_start_daemon ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$client_start_daemon ();
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Vec< ::std::int32_t>
client_handle_fd_argument (::rust::Str arg, ::rust::Str arch, bool is_replace)
{
  ::rust::MaybeUninit< ::rust::Vec< ::std::int32_t> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$client_handle_fd_argument (arg, arch, is_replace, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
client_render_download_progress (const ::rpmostreecxx::GVariant &progress) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$client_render_download_progress (progress, &return$.value);
  return ::std::move (return$.value);
}

bool
running_in_container () noexcept
{
  return rpmostreecxx$cxxbridge1$running_in_container ();
}

bool
confirm ()
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$confirm (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
confirm_or_abort ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$confirm_or_abort ();
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::std::size_t
Bubblewrap::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$Bubblewrap$operator$sizeof ();
}

::std::size_t
Bubblewrap::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$Bubblewrap$operator$alignof ();
}

void
bubblewrap_selftest ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$bubblewrap_selftest ();
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Vec< ::std::uint8_t>
bubblewrap_run_sync (::std::int32_t rootfs_dfd, const ::rust::Vec< ::rust::String> &args,
                     bool capture_stdout, bool unified_core)
{
  ::rust::MaybeUninit< ::rust::Vec< ::std::uint8_t> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$bubblewrap_run_sync (
      rootfs_dfd, args, capture_stdout, unified_core, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Bubblewrap>
bubblewrap_new (::std::int32_t rootfs_fd)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Bubblewrap> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$bubblewrap_new (rootfs_fd, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Bubblewrap>
bubblewrap_new_with_mutability (::std::int32_t rootfs_fd,
                                ::rpmostreecxx::BubblewrapMutability mutability)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Bubblewrap> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$bubblewrap_new_with_mutability (
      rootfs_fd, mutability, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::int32_t
Bubblewrap::get_rootfs_fd () const noexcept
{
  return rpmostreecxx$cxxbridge1$Bubblewrap$get_rootfs_fd (*this);
}

void
Bubblewrap::append_bwrap_arg (::rust::Str arg) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$append_bwrap_arg (*this, arg);
}

void
Bubblewrap::append_child_arg (::rust::Str arg) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$append_child_arg (*this, arg);
}

void
Bubblewrap::setenv (::rust::Str k, ::rust::Str v) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$setenv (*this, k, v);
}

void
Bubblewrap::take_fd (::std::int32_t source_fd, ::std::int32_t target_fd) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$take_fd (*this, source_fd, target_fd);
}

void
Bubblewrap::set_inherit_stdin () noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$set_inherit_stdin (*this);
}

void
Bubblewrap::take_stdin_fd (::std::int32_t source_fd) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$take_stdin_fd (*this, source_fd);
}

void
Bubblewrap::take_stdout_fd (::std::int32_t source_fd) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$take_stdout_fd (*this, source_fd);
}

void
Bubblewrap::take_stderr_fd (::std::int32_t source_fd) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$take_stderr_fd (*this, source_fd);
}

void
Bubblewrap::take_stdout_and_stderr_fd (::std::int32_t source_fd) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$take_stdout_and_stderr_fd (*this, source_fd);
}

void
Bubblewrap::bind_read (::rust::Str src, ::rust::Str dest) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$bind_read (*this, src, dest);
}

void
Bubblewrap::bind_readwrite (::rust::Str src, ::rust::Str dest) noexcept
{
  rpmostreecxx$cxxbridge1$Bubblewrap$bind_readwrite (*this, src, dest);
}

void
Bubblewrap::setup_compat_var ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Bubblewrap$setup_compat_var (*this);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
Bubblewrap::run (const ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Bubblewrap$run (*this, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
applylive_entrypoint (const ::rust::Vec< ::rust::String> &args)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$applylive_entrypoint (args);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
applylive_finish (const ::rpmostreecxx::OstreeSysroot &sysroot)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$applylive_finish (sysroot);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
composeutil_legacy_prep_dev_and_run (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$composeutil_legacy_prep_dev_and_run (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
print_ostree_txn_stats (::rpmostreecxx::OstreeRepoTransactionStats &stats) noexcept
{
  rpmostreecxx$cxxbridge1$print_ostree_txn_stats (stats);
}

void
write_commit_id (::rust::Str target_path, ::rust::Str revision)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$write_commit_id (target_path, revision);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
cliwrap_write_wrappers (::std::int32_t rootfs)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$cliwrap_write_wrappers (rootfs);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
cliwrap_write_some_wrappers (::std::int32_t rootfs, const ::rust::Vec< ::rust::String> &args)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$cliwrap_write_some_wrappers (rootfs, args);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::String
cliwrap_destdir () noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$cliwrap_destdir (&return$.value);
  return ::std::move (return$.value);
}

void
container_encapsulate (::rust::Vec< ::rust::String> args)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > args$ (::std::move (args));
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$container_encapsulate (&args$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
deploy_from_self_entrypoint (::rust::Vec< ::rust::String> args)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > args$ (::std::move (args));
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$deploy_from_self_entrypoint (&args$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::ContainerImageState>
pull_container (const ::rpmostreecxx::OstreeRepo &repo,
                const ::rpmostreecxx::GCancellable &cancellable, ::rust::Str imgref)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::ContainerImageState> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$pull_container (repo, cancellable, imgref, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
container_prune (const ::rpmostreecxx::OstreeRepo &repo,
                 const ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$container_prune (repo, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::ContainerImageState>
query_container_image_commit (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str c)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::ContainerImageState> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$query_container_image_commit (repo, c, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
purge_refspec (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str refspec)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$purge_refspec (repo, refspec);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::std::size_t
TempEtcGuard::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$TempEtcGuard$operator$sizeof ();
}

::std::size_t
TempEtcGuard::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$TempEtcGuard$operator$alignof ();
}

::std::size_t
FilesystemScriptPrep::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$FilesystemScriptPrep$operator$sizeof ();
}

::std::size_t
FilesystemScriptPrep::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$FilesystemScriptPrep$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::TempEtcGuard>
prepare_tempetc_guard (::std::int32_t rootfs)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::TempEtcGuard> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$prepare_tempetc_guard (rootfs, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
TempEtcGuard::undo () const
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$TempEtcGuard$undo (*this);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::FilesystemScriptPrep>
prepare_filesystem_script_prep (::std::int32_t rootfs)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::FilesystemScriptPrep> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$prepare_filesystem_script_prep (rootfs, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
FilesystemScriptPrep::undo ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$FilesystemScriptPrep$undo (*this);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
run_depmod (::std::int32_t rootfs_dfd, ::rust::Str kver, bool unified_core)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$run_depmod (rootfs_dfd, kver, unified_core);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
log_treefile (const ::rpmostreecxx::Treefile &tf) noexcept
{
  rpmostreecxx$cxxbridge1$log_treefile (tf);
}

bool
is_container_image_reference (::rust::Str refspec) noexcept
{
  return rpmostreecxx$cxxbridge1$is_container_image_reference (refspec);
}

::rpmostreecxx::RefspecType
refspec_classify (::rust::Str refspec) noexcept
{
  return rpmostreecxx$cxxbridge1$refspec_classify (refspec);
}

void
verify_kernel_hmac (::std::int32_t rootfs, ::rust::Str moddir)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$verify_kernel_hmac (rootfs, moddir);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Vec< ::rust::String>
stage_container_rpms (::rust::Vec< ::rust::String> rpms)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > rpms$ (::std::move (rpms));
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$stage_container_rpms (&rpms$.value, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
stage_container_rpm_raw_fds (::rust::Vec< ::std::int32_t> fds)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::std::int32_t> > fds$ (::std::move (fds));
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$stage_container_rpm_raw_fds (&fds$.value, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
commit_has_matching_sepolicy (const ::rpmostreecxx::GVariant &commit,
                              const ::rpmostreecxx::OstreeSePolicy &policy)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$commit_has_matching_sepolicy (commit, policy, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::GVariant *
get_header_variant (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str cachebranch)
{
  ::rust::MaybeUninit< ::rpmostreecxx::GVariant *> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$get_header_variant (repo, cachebranch, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
compose_image (::rust::Vec< ::rust::String> args)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > args$ (::std::move (args));
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$compose_image (&args$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
configure_build_repo_from_target (const ::rpmostreecxx::OstreeRepo &build_repo,
                                  const ::rpmostreecxx::OstreeRepo &target_repo)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$configure_build_repo_from_target (build_repo, target_repo);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
compose_prepare_rootfs (::std::int32_t src_rootfs_dfd, ::std::int32_t dest_rootfs_dfd,
                        ::rpmostreecxx::Treefile &treefile)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$compose_prepare_rootfs (src_rootfs_dfd, dest_rootfs_dfd, treefile);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
composepost_nsswitch_altfiles (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$composepost_nsswitch_altfiles (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
compose_postprocess (::std::int32_t rootfs_dfd, ::rpmostreecxx::Treefile &treefile,
                     ::rust::Str next_version, bool unified_core)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$compose_postprocess (
      rootfs_dfd, treefile, next_version, unified_core);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
compose_postprocess_final (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$compose_postprocess_final (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
convert_var_to_tmpfiles_d (::std::int32_t rootfs_dfd,
                           const ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$convert_var_to_tmpfiles_d (rootfs_dfd, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
rootfs_prepare_links (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$rootfs_prepare_links (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
workaround_selinux_cross_labeling (::std::int32_t rootfs_dfd,
                                   ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$workaround_selinux_cross_labeling (rootfs_dfd, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
prepare_rpmdb_base_location (::std::int32_t rootfs_dfd, ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$prepare_rpmdb_base_location (rootfs_dfd, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
compose_postprocess_rpm_macro (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$compose_postprocess_rpm_macro (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
postprocess_cleanup_rpmdb (::std::int32_t rootfs_dfd)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$postprocess_cleanup_rpmdb (rootfs_dfd);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
rewrite_rpmdb_for_target (::std::int32_t rootfs_dfd, bool normalize)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$rewrite_rpmdb_for_target (rootfs_dfd, normalize);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::std::uint64_t
directory_size (::std::int32_t dfd, const ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::MaybeUninit< ::std::uint64_t> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$directory_size (dfd, cancellable, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::OstreeDeployment *
deployment_for_id (::rpmostreecxx::OstreeSysroot &sysroot, ::rust::Str deploy_id)
{
  ::rust::MaybeUninit< ::rpmostreecxx::OstreeDeployment *> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$deployment_for_id (sysroot, deploy_id, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
deployment_checksum_for_id (::rpmostreecxx::OstreeSysroot &sysroot, ::rust::Str deploy_id)
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$deployment_checksum_for_id (sysroot, deploy_id, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::OstreeDeployment *
deployment_get_base (::rpmostreecxx::OstreeSysroot &sysroot, ::rust::Str opt_deploy_id,
                     ::rust::Str opt_os_name)
{
  ::rust::MaybeUninit< ::rpmostreecxx::OstreeDeployment *> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$deployment_get_base (
      sysroot, opt_deploy_id, opt_os_name, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
daemon_main (bool debug)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$daemon_main (debug);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
daemon_terminate () noexcept
{
  rpmostreecxx$cxxbridge1$daemon_terminate ();
}

void
daemon_sanitycheck_environment (const ::rpmostreecxx::OstreeSysroot &sysroot)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$daemon_sanitycheck_environment (sysroot);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::String
deployment_generate_id (const ::rpmostreecxx::OstreeDeployment &deployment) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$deployment_generate_id (deployment, &return$.value);
  return ::std::move (return$.value);
}

void
deployment_populate_variant (const ::rpmostreecxx::OstreeSysroot &sysroot,
                             const ::rpmostreecxx::OstreeDeployment &deployment,
                             const ::rpmostreecxx::GVariantDict &dict)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$deployment_populate_variant (sysroot, deployment, dict);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
generate_baselayer_refs (const ::rpmostreecxx::OstreeSysroot &sysroot,
                         const ::rpmostreecxx::OstreeRepo &repo,
                         const ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$generate_baselayer_refs (sysroot, repo, cancellable);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
variant_add_remote_status (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str refspec,
                           ::rust::Str base_checksum, const ::rpmostreecxx::GVariantDict &dict)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$variant_add_remote_status (repo, refspec, base_checksum, dict);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rpmostreecxx::DeploymentLayeredMeta
deployment_layeredmeta_from_commit (const ::rpmostreecxx::OstreeDeployment &deployment,
                                    const ::rpmostreecxx::GVariant &commit)
{
  ::rust::MaybeUninit< ::rpmostreecxx::DeploymentLayeredMeta> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$deployment_layeredmeta_from_commit (
      deployment, commit, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::DeploymentLayeredMeta
deployment_layeredmeta_load (const ::rpmostreecxx::OstreeRepo &repo,
                             const ::rpmostreecxx::OstreeDeployment &deployment)
{
  ::rust::MaybeUninit< ::rpmostreecxx::DeploymentLayeredMeta> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$deployment_layeredmeta_load (repo, deployment, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::OverrideReplacementSource
parse_override_source (::rust::Str source)
{
  ::rust::MaybeUninit< ::rpmostreecxx::OverrideReplacementSource> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$parse_override_source (source, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::ParsedRevision
parse_revision (::rust::Str source)
{
  ::rust::MaybeUninit< ::rpmostreecxx::ParsedRevision> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$parse_revision (source, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
generate_object_path (::rust::Str base, ::rust::Str next_segment)
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$generate_object_path (base, next_segment, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
failpoint (::rust::Str p)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$failpoint (p);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::std::size_t
RpmImporterFlags::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporterFlags$operator$sizeof ();
}

::std::size_t
RpmImporterFlags::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporterFlags$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::RpmImporterFlags>
rpm_importer_flags_new_empty () noexcept
{
  return ::rust::Box< ::rpmostreecxx::RpmImporterFlags>::from_raw (
      rpmostreecxx$cxxbridge1$rpm_importer_flags_new_empty ());
}

bool
RpmImporterFlags::is_ima_enabled () const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporterFlags$is_ima_enabled (*this);
}

::std::size_t
RpmImporter::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$operator$sizeof ();
}

::std::size_t
RpmImporter::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::RpmImporter>
rpm_importer_new (::rust::Str pkg_name, ::rust::Str ostree_branch,
                  const ::rpmostreecxx::RpmImporterFlags &flags)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::RpmImporter> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$rpm_importer_new (pkg_name, ostree_branch, flags, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
RpmImporter::handle_translate_pathname (::rust::Str path) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$RpmImporter$handle_translate_pathname (*this, path, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
RpmImporter::ostree_branch () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$RpmImporter$ostree_branch (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
RpmImporter::pkg_name () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$RpmImporter$pkg_name (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
RpmImporter::doc_files_are_filtered () const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$doc_files_are_filtered (*this);
}

void
RpmImporter::doc_files_insert (::rust::Str path) noexcept
{
  rpmostreecxx$cxxbridge1$RpmImporter$doc_files_insert (*this, path);
}

bool
RpmImporter::doc_files_contains (::rust::Str path) const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$doc_files_contains (*this, path);
}

void
RpmImporter::rpmfi_overrides_insert (::rust::Str path, ::std::uint64_t index) noexcept
{
  rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_insert (*this, path, index);
}

bool
RpmImporter::rpmfi_overrides_contains (::rust::Str path) const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_contains (*this, path);
}

::std::uint64_t
RpmImporter::rpmfi_overrides_get (::rust::Str path) const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$rpmfi_overrides_get (*this, path);
}

bool
RpmImporter::is_ima_enabled () const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$is_ima_enabled (*this);
}

void
RpmImporter::tweak_imported_file_info (const ::rpmostreecxx::GFileInfo &file_info) const noexcept
{
  rpmostreecxx$cxxbridge1$RpmImporter$tweak_imported_file_info (*this, file_info);
}

bool
RpmImporter::is_file_filtered (::rust::Str path, const ::rpmostreecxx::GFileInfo &file_info) const
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$RpmImporter$is_file_filtered (
      *this, path, file_info, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
RpmImporter::translate_to_tmpfiles_entry (::rust::Str abs_path,
                                          const ::rpmostreecxx::GFileInfo &file_info,
                                          ::rust::Str username, ::rust::Str groupname)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$RpmImporter$translate_to_tmpfiles_entry (
      *this, abs_path, file_info, username, groupname);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
RpmImporter::has_tmpfiles_entries () const noexcept
{
  return rpmostreecxx$cxxbridge1$RpmImporter$has_tmpfiles_entries (*this);
}

::rust::String
RpmImporter::serialize_tmpfiles_content () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$RpmImporter$serialize_tmpfiles_content (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
tmpfiles_translate (::rust::Str abs_path, const ::rpmostreecxx::GFileInfo &file_info,
                    ::rust::Str username, ::rust::Str groupname)
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$tmpfiles_translate (
      abs_path, file_info, username, groupname, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Slice<const ::std::uint8_t>
get_dracut_random_cpio () noexcept
{
  return ::rust::impl< ::rust::Slice<const ::std::uint8_t> >::slice (
      rpmostreecxx$cxxbridge1$get_dracut_random_cpio ());
}

::std::int32_t
initramfs_overlay_generate (const ::rust::Vec< ::rust::String> &files,
                            ::rpmostreecxx::GCancellable &cancellable)
{
  ::rust::MaybeUninit< ::std::int32_t> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$initramfs_overlay_generate (files, cancellable, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
journal_print_staging_failure () noexcept
{
  rpmostreecxx$cxxbridge1$journal_print_staging_failure ();
}

void
console_progress_begin_task (::rust::Str msg) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_begin_task (msg);
}

void
console_progress_begin_n_items (::rust::Str msg, ::std::uint64_t n) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_begin_n_items (msg, n);
}

void
console_progress_begin_percent (::rust::Str msg) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_begin_percent (msg);
}

void
console_progress_set_message (::rust::Str msg) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_set_message (msg);
}

void
console_progress_set_sub_message (::rust::Str msg) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_set_sub_message (msg);
}

void
console_progress_update (::std::uint64_t n) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_update (n);
}

void
console_progress_end (::rust::Str suffix) noexcept
{
  rpmostreecxx$cxxbridge1$console_progress_end (suffix);
}

bool
HistoryEntry::operator== (const HistoryEntry &rhs) const noexcept
{
  return rpmostreecxx$cxxbridge1$HistoryEntry$operator$eq (*this, rhs);
}

bool
HistoryEntry::operator!= (const HistoryEntry &rhs) const noexcept
{
  return rpmostreecxx$cxxbridge1$HistoryEntry$operator$ne (*this, rhs);
}

::std::size_t
HistoryCtx::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$HistoryCtx$operator$sizeof ();
}

::std::size_t
HistoryCtx::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$HistoryCtx$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::HistoryCtx>
history_ctx_new ()
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::HistoryCtx> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$history_ctx_new (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::HistoryEntry
HistoryCtx::next_entry ()
{
  ::rust::MaybeUninit< ::rpmostreecxx::HistoryEntry> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$HistoryCtx$next_entry (*this, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
history_prune ()
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$history_prune ();
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
modularity_entrypoint (const ::rust::Vec< ::rust::String> &args)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$modularity_entrypoint (args);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::std::size_t
TokioHandle::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$TokioHandle$operator$sizeof ();
}

::std::size_t
TokioHandle::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$TokioHandle$operator$alignof ();
}

::std::size_t
TokioEnterGuard::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$TokioEnterGuard$operator$sizeof ();
}

::std::size_t
TokioEnterGuard::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$TokioEnterGuard$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::TokioHandle>
tokio_handle_get () noexcept
{
  return ::rust::Box< ::rpmostreecxx::TokioHandle>::from_raw (
      rpmostreecxx$cxxbridge1$tokio_handle_get ());
}

::rust::Box< ::rpmostreecxx::TokioEnterGuard>
TokioHandle::enter () const noexcept
{
  return ::rust::Box< ::rpmostreecxx::TokioEnterGuard>::from_raw (
      rpmostreecxx$cxxbridge1$TokioHandle$enter (*this));
}

bool
script_is_ignored (::rust::Str pkg, ::rust::Str script) noexcept
{
  return rpmostreecxx$cxxbridge1$script_is_ignored (pkg, script);
}

void
testutils_entrypoint (::rust::Vec< ::rust::String> argv)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > argv$ (::std::move (argv));
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$testutils_entrypoint (&argv$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::String
maybe_shell_quote (::rust::Str input) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$maybe_shell_quote (input, &return$.value);
  return ::std::move (return$.value);
}

bool
Refspec::operator== (const Refspec &rhs) const noexcept
{
  return rpmostreecxx$cxxbridge1$Refspec$operator$eq (*this, rhs);
}

bool
Refspec::operator!= (const Refspec &rhs) const noexcept
{
  return !(*this == rhs);
}

bool
OverrideReplacement::operator== (const OverrideReplacement &rhs) const noexcept
{
  return rpmostreecxx$cxxbridge1$OverrideReplacement$operator$eq (*this, rhs);
}

bool
OverrideReplacement::operator!= (const OverrideReplacement &rhs) const noexcept
{
  return !(*this == rhs);
}

::std::size_t
Treefile::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$operator$sizeof ();
}

::std::size_t
Treefile::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new (::rust::Str filename, ::rust::Str basearch)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$treefile_new (filename, basearch, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new_empty ()
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$treefile_new_empty (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new_from_string (::rust::Str buf, bool client)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$treefile_new_from_string (buf, client, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new_compose (::rust::Str filename, ::rust::Str basearch)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$treefile_new_compose (filename, basearch, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new_client (::rust::Str filename, ::rust::Str basearch)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$treefile_new_client (filename, basearch, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
treefile_new_client_from_etc (::rust::Str basearch)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$treefile_new_client_from_etc (basearch, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::uint32_t
treefile_delete_client_etc ()
{
  ::rust::MaybeUninit< ::std::uint32_t> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$treefile_delete_client_etc (&return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Str
Treefile::get_workdir () const noexcept
{
  return ::rust::impl< ::rust::Str>::new_unchecked (
      rpmostreecxx$cxxbridge1$Treefile$get_workdir (*this));
}

::std::int32_t
Treefile::get_passwd_fd () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_passwd_fd (*this);
}

::std::int32_t
Treefile::get_group_fd () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_group_fd (*this);
}

::rust::String
Treefile::get_json_string () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_json_string (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_ostree_layers () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_ostree_layers (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_ostree_override_layers () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_ostree_override_layers (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_all_ostree_layers () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_all_ostree_layers (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_repos () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_repos (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::require_automatic_version_prefix () const
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$require_automatic_version_prefix (*this, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
Treefile::add_packages (::rust::Vec< ::rust::String> packages, bool allow_existing)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$add_packages (
      *this, &packages$.value, allow_existing, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
Treefile::has_packages () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_packages (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_local_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_local_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::add_local_packages (::rust::Vec< ::rust::String> packages, bool allow_existing)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$add_local_packages (
      *this, &packages$.value, allow_existing, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_local_fileoverride_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_local_fileoverride_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::add_local_fileoverride_packages (::rust::Vec< ::rust::String> packages,
                                           bool allow_existing)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$add_local_fileoverride_packages (
      *this, &packages$.value, allow_existing, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
Treefile::remove_packages (::rust::Vec< ::rust::String> packages, bool allow_noent)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$remove_packages (
      *this, &packages$.value, allow_noent, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Vec< ::rpmostreecxx::OverrideReplacement>
Treefile::get_packages_override_replace () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rpmostreecxx::OverrideReplacement> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_packages_override_replace (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::has_packages_override_replace () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_packages_override_replace (*this);
}

bool
Treefile::add_packages_override_replace (::rpmostreecxx::OverrideReplacement replacement) noexcept
{
  ::rust::ManuallyDrop< ::rpmostreecxx::OverrideReplacement> replacement$ (
      ::std::move (replacement));
  return rpmostreecxx$cxxbridge1$Treefile$add_packages_override_replace (*this,
                                                                         &replacement$.value);
}

bool
Treefile::remove_package_override_replace (::rust::Str package) noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$remove_package_override_replace (*this, package);
}

::rust::Vec< ::rust::String>
Treefile::get_packages_override_replace_local () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_packages_override_replace_local (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::add_packages_override_replace_local (::rust::Vec< ::rust::String> packages)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$add_packages_override_replace_local (*this,
                                                                              &packages$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
Treefile::remove_package_override_replace_local (::rust::Str package) noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$remove_package_override_replace_local (*this, package);
}

::rust::Vec< ::rust::String>
Treefile::get_packages_override_remove () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_packages_override_remove (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::add_packages_override_remove (::rust::Vec< ::rust::String> packages)
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > packages$ (::std::move (packages));
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$add_packages_override_remove (*this, &packages$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
Treefile::remove_package_override_remove (::rust::Str package) noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$remove_package_override_remove (*this, package);
}

bool
Treefile::has_packages_override_remove_name (::rust::Str name) const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_packages_override_remove_name (*this, name);
}

bool
Treefile::remove_all_overrides () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$remove_all_overrides (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_modules_enable () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_modules_enable (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::has_modules_enable () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_modules_enable (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_modules_install () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_modules_install (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::add_modules (::rust::Vec< ::rust::String> modules, bool enable_only) noexcept
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > modules$ (::std::move (modules));
  return rpmostreecxx$cxxbridge1$Treefile$add_modules (*this, &modules$.value, enable_only);
}

bool
Treefile::remove_modules (::rust::Vec< ::rust::String> modules, bool enable_only) noexcept
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > modules$ (::std::move (modules));
  return rpmostreecxx$cxxbridge1$Treefile$remove_modules (*this, &modules$.value, enable_only);
}

bool
Treefile::remove_all_packages () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$remove_all_packages (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_exclude_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_exclude_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_platform_module () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_platform_module (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_install_langs () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_install_langs (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::format_install_langs_macro () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$format_install_langs_macro (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Treefile::get_lockfile_repos () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_lockfile_repos (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Str
Treefile::get_ref () const noexcept
{
  return ::rust::impl< ::rust::Str>::new_unchecked (
      rpmostreecxx$cxxbridge1$Treefile$get_ref (*this));
}

bool
Treefile::get_cliwrap () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_cliwrap (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_cliwrap_binaries () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_cliwrap_binaries (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::set_cliwrap (bool enabled) noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$set_cliwrap (*this, enabled);
}

::rust::Vec< ::rust::String>
Treefile::get_container_cmd () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_container_cmd (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::get_readonly_executables () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_readonly_executables (*this);
}

bool
Treefile::get_documentation () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_documentation (*this);
}

bool
Treefile::get_recommends () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_recommends (*this);
}

bool
Treefile::get_selinux () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_selinux (*this);
}

::rust::String
Treefile::get_gpg_key () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_gpg_key (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_automatic_version_suffix () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_automatic_version_suffix (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::get_container () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_container (*this);
}

bool
Treefile::get_machineid_compat () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_machineid_compat (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_etc_group_members () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_etc_group_members (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::get_boot_location_is_modules () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_boot_location_is_modules (*this);
}

bool
Treefile::get_ima () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_ima (*this);
}

::rust::String
Treefile::get_releasever () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_releasever (*this, &return$.value);
  return ::std::move (return$.value);
}

::rpmostreecxx::RepoMetadataTarget
Treefile::get_repo_metadata_target () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_repo_metadata_target (*this);
}

bool
Treefile::rpmdb_backend_is_target () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$rpmdb_backend_is_target (*this);
}

bool
Treefile::should_normalize_rpmdb () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$should_normalize_rpmdb (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_files_remove_regex (::rust::Str package) const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_files_remove_regex (*this, package, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_checksum (const ::rpmostreecxx::OstreeRepo &repo) const
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$get_checksum (*this, repo, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_ostree_ref () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_ostree_ref (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Slice<const ::rpmostreecxx::RepoPackage>
Treefile::get_repo_packages () const noexcept
{
  return ::rust::impl< ::rust::Slice<const ::rpmostreecxx::RepoPackage> >::slice (
      rpmostreecxx$cxxbridge1$Treefile$get_repo_packages (*this));
}

void
Treefile::clear_repo_packages () noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$clear_repo_packages (*this);
}

void
Treefile::prettyprint_json_stdout () const noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$prettyprint_json_stdout (*this);
}

void
Treefile::print_deprecation_warnings () const noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$print_deprecation_warnings (*this);
}

void
Treefile::print_experimental_notices () const noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$print_experimental_notices (*this);
}

void
Treefile::sanitycheck_externals () const
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$sanitycheck_externals (*this);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::RpmImporterFlags>
Treefile::importer_flags (::rust::Str pkg_name) const noexcept
{
  return ::rust::Box< ::rpmostreecxx::RpmImporterFlags>::from_raw (
      rpmostreecxx$cxxbridge1$Treefile$importer_flags (*this, pkg_name));
}

::rust::String
Treefile::write_repovars (::std::int32_t workdir_dfd_raw) const
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$write_repovars (*this, workdir_dfd_raw, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
Treefile::set_releasever (::rust::Str releasever)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$set_releasever (*this, releasever);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
Treefile::enable_repo (::rust::Str repo)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$enable_repo (*this, repo);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
Treefile::disable_repo (::rust::Str repo)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$disable_repo (*this, repo);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
Treefile::validate_for_container () const
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Treefile$validate_for_container (*this);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rpmostreecxx::Refspec
Treefile::get_base_refspec () const noexcept
{
  ::rust::MaybeUninit< ::rpmostreecxx::Refspec> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_base_refspec (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::rebase (::rust::Str new_refspec, ::rust::Str custom_origin_url,
                  ::rust::Str custom_origin_description) noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$rebase (*this, new_refspec, custom_origin_url,
                                           custom_origin_description);
}

::rust::String
Treefile::get_origin_custom_url () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_origin_custom_url (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_origin_custom_description () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_origin_custom_description (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
Treefile::get_override_commit () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_override_commit (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::set_override_commit (::rust::Str checksum) noexcept
{
  rpmostreecxx$cxxbridge1$Treefile$set_override_commit (*this, checksum);
}

::rust::Vec< ::rust::String>
Treefile::get_initramfs_etc_files () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_initramfs_etc_files (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::has_initramfs_etc_files () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_initramfs_etc_files (*this);
}

bool
Treefile::initramfs_etc_files_track (::rust::Vec< ::rust::String> files) noexcept
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > files$ (::std::move (files));
  return rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_track (*this, &files$.value);
}

bool
Treefile::initramfs_etc_files_untrack (::rust::Vec< ::rust::String> files) noexcept
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > files$ (::std::move (files));
  return rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_untrack (*this, &files$.value);
}

bool
Treefile::initramfs_etc_files_untrack_all () noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$initramfs_etc_files_untrack_all (*this);
}

bool
Treefile::get_initramfs_regenerate () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$get_initramfs_regenerate (*this);
}

::rust::Vec< ::rust::String>
Treefile::get_initramfs_args () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Treefile$get_initramfs_args (*this, &return$.value);
  return ::std::move (return$.value);
}

void
Treefile::set_initramfs_regenerate (bool enabled, ::rust::Vec< ::rust::String> args) noexcept
{
  ::rust::ManuallyDrop< ::rust::Vec< ::rust::String> > args$ (::std::move (args));
  rpmostreecxx$cxxbridge1$Treefile$set_initramfs_regenerate (*this, enabled, &args$.value);
}

::rust::String
Treefile::get_unconfigured_state () const noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$Treefile$get_unconfigured_state (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Treefile::may_require_local_assembly () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$may_require_local_assembly (*this);
}

bool
Treefile::has_any_packages () const noexcept
{
  return rpmostreecxx$cxxbridge1$Treefile$has_any_packages (*this);
}

bool
Treefile::merge_treefile (::rust::Str treefile)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Treefile$merge_treefile (*this, treefile, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::size_t
RepoPackage::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$RepoPackage$operator$sizeof ();
}

::std::size_t
RepoPackage::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$RepoPackage$operator$alignof ();
}

::rust::Str
RepoPackage::get_repo () const noexcept
{
  return ::rust::impl< ::rust::Str>::new_unchecked (
      rpmostreecxx$cxxbridge1$RepoPackage$get_repo (*this));
}

::rust::Vec< ::rust::String>
RepoPackage::get_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$RepoPackage$get_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::String
varsubstitute (::rust::Str s, const ::rust::Vec< ::rpmostreecxx::StringMapping> &vars)
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$varsubstitute (s, vars, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
get_features () noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$get_features (&return$.value);
  return ::std::move (return$.value);
}

::rust::String
get_rpm_basearch () noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$get_rpm_basearch (&return$.value);
  return ::std::move (return$.value);
}

::std::int32_t
sealed_memfd (::rust::Str description, ::rust::Slice<const ::std::uint8_t> content)
{
  ::rust::MaybeUninit< ::std::int32_t> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$sealed_memfd (description, content, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
running_in_systemd () noexcept
{
  return rpmostreecxx$cxxbridge1$running_in_systemd ();
}

::rpmostreecxx::GVariant *
calculate_advisories_diff (const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str checksum_from,
                           ::rust::Str checksum_to)
{
  ::rust::MaybeUninit< ::rpmostreecxx::GVariant *> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$calculate_advisories_diff (
      repo, checksum_from, checksum_to, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
translate_path_for_ostree (::rust::Str path) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$translate_path_for_ostree (path, &return$.value);
  return ::std::move (return$.value);
}

::rpmostreecxx::LiveApplyState
get_live_apply_state (const ::rpmostreecxx::OstreeSysroot &sysroot,
                      const ::rpmostreecxx::OstreeDeployment &deployment)
{
  ::rust::MaybeUninit< ::rpmostreecxx::LiveApplyState> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$get_live_apply_state (sysroot, deployment, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
has_live_apply_state (const ::rpmostreecxx::OstreeSysroot &sysroot,
                      const ::rpmostreecxx::OstreeDeployment &deployment)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$has_live_apply_state (sysroot, deployment, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
applylive_sync_ref (const ::rpmostreecxx::OstreeSysroot &sysroot)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$applylive_sync_ref (sysroot);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
transaction_apply_live (const ::rpmostreecxx::OstreeSysroot &sysroot,
                        const ::rpmostreecxx::GVariant &target)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$transaction_apply_live (sysroot, target);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
prepare_rpm_layering (::std::int32_t rootfs, ::rust::Str merge_passwd_dir)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$prepare_rpm_layering (rootfs, merge_passwd_dir, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
complete_rpm_layering (::std::int32_t rootfs)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$complete_rpm_layering (rootfs);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
passwd_cleanup (::std::int32_t rootfs)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$passwd_cleanup (rootfs);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
migrate_group_except_root (::std::int32_t rootfs,
                           const ::rust::Vec< ::rust::String> &preserved_groups)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$migrate_group_except_root (rootfs, preserved_groups);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
migrate_passwd_except_root (::std::int32_t rootfs)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$migrate_passwd_except_root (rootfs);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
passwd_compose_prep (::std::int32_t rootfs, ::rpmostreecxx::Treefile &treefile)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$passwd_compose_prep (rootfs, treefile);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
passwd_compose_prep_repo (::std::int32_t rootfs, ::rpmostreecxx::Treefile &treefile,
                          const ::rpmostreecxx::OstreeRepo &repo, ::rust::Str previous_checksum,
                          bool unified_core)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$passwd_compose_prep_repo (
      rootfs, treefile, repo, previous_checksum, unified_core);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
dir_contains_uid (::std::int32_t dirfd, ::std::uint32_t id)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$dir_contains_uid (dirfd, id, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

bool
dir_contains_gid (::std::int32_t dirfd, ::std::uint32_t id)
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$dir_contains_gid (dirfd, id, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
check_passwd_group_entries (const ::rpmostreecxx::OstreeRepo &ffi_repo, ::std::int32_t rootfs_dfd,
                            ::rpmostreecxx::Treefile &treefile, ::rust::Str previous_rev)
{
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$check_passwd_group_entries (
      ffi_repo, rootfs_dfd, treefile, previous_rev);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::PasswdDB>
passwddb_open (::std::int32_t rootfs)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::PasswdDB> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$passwddb_open (rootfs, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::size_t
PasswdDB::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdDB$operator$sizeof ();
}

::std::size_t
PasswdDB::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdDB$operator$alignof ();
}

::rust::String
PasswdDB::lookup_user (::std::uint32_t uid) const
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdDB$lookup_user (*this, uid, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::String
PasswdDB::lookup_group (::std::uint32_t gid) const
{
  ::rust::MaybeUninit< ::rust::String> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdDB$lookup_group (*this, gid, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::PasswdEntries>
new_passwd_entries () noexcept
{
  return ::rust::Box< ::rpmostreecxx::PasswdEntries>::from_raw (
      rpmostreecxx$cxxbridge1$new_passwd_entries ());
}

::std::size_t
PasswdEntries::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdEntries$operator$sizeof ();
}

::std::size_t
PasswdEntries::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdEntries$operator$alignof ();
}

void
PasswdEntries::add_group_content (::std::int32_t rootfs, ::rust::Str path)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdEntries$add_group_content (*this, rootfs, path);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
PasswdEntries::add_passwd_content (::std::int32_t rootfs, ::rust::Str path)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdEntries$add_passwd_content (*this, rootfs, path);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

bool
PasswdEntries::contains_group (::rust::Str user) const noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdEntries$contains_group (*this, user);
}

bool
PasswdEntries::contains_user (::rust::Str user) const noexcept
{
  return rpmostreecxx$cxxbridge1$PasswdEntries$contains_user (*this, user);
}

::std::uint32_t
PasswdEntries::lookup_user_id (::rust::Str user) const
{
  ::rust::MaybeUninit< ::std::uint32_t> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdEntries$lookup_user_id (*this, user, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::uint32_t
PasswdEntries::lookup_group_id (::rust::Str group) const
{
  ::rust::MaybeUninit< ::std::uint32_t> return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$PasswdEntries$lookup_group_id (*this, group, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::size_t
Extensions::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$Extensions$operator$sizeof ();
}

::std::size_t
Extensions::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$Extensions$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::Extensions>
extensions_load (::rust::Str path, ::rust::Str basearch,
                 const ::rust::Vec< ::rpmostreecxx::StringMapping> &base_pkgs)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Extensions> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$extensions_load (path, basearch, base_pkgs, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Extensions::get_repos () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Extensions$get_repos (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Extensions::get_os_extension_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Extensions$get_os_extension_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

::rust::Vec< ::rust::String>
Extensions::get_development_packages () const noexcept
{
  ::rust::MaybeUninit< ::rust::Vec< ::rust::String> > return$;
  rpmostreecxx$cxxbridge1$Extensions$get_development_packages (*this, &return$.value);
  return ::std::move (return$.value);
}

bool
Extensions::state_checksum_changed (::rust::Str chksum, ::rust::Str output_dir) const
{
  ::rust::MaybeUninit<bool> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$Extensions$state_checksum_changed (
      *this, chksum, output_dir, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
Extensions::update_state_checksum (::rust::Str chksum, ::rust::Str output_dir) const
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Extensions$update_state_checksum (*this, chksum, output_dir);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

void
Extensions::serialize_to_dir (::rust::Str output_dir) const
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Extensions$serialize_to_dir (*this, output_dir);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Box< ::rpmostreecxx::Treefile>
Extensions::generate_treefile (const ::rpmostreecxx::Treefile &src) const
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$Extensions$generate_treefile (*this, src, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::std::size_t
LockfileConfig::layout::size () noexcept
{
  return rpmostreecxx$cxxbridge1$LockfileConfig$operator$sizeof ();
}

::std::size_t
LockfileConfig::layout::align () noexcept
{
  return rpmostreecxx$cxxbridge1$LockfileConfig$operator$alignof ();
}

::rust::Box< ::rpmostreecxx::LockfileConfig>
lockfile_read (const ::rust::Vec< ::rust::String> &filenames)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::LockfileConfig> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$lockfile_read (filenames, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
lockfile_write (::rust::Str filename, ::rpmostreecxx::CxxGObjectArray &packages,
                ::rpmostreecxx::CxxGObjectArray &rpmmd_repos)
{
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$lockfile_write (filename, packages, rpmmd_repos);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
}

::rust::Vec< ::rpmostreecxx::LockedPackage>
LockfileConfig::get_locked_packages () const
{
  ::rust::MaybeUninit< ::rust::Vec< ::rpmostreecxx::LockedPackage> > return$;
  ::rust::repr::PtrLen error$
      = rpmostreecxx$cxxbridge1$LockfileConfig$get_locked_packages (*this, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rust::Box< ::rpmostreecxx::Treefile>
origin_to_treefile (const ::rpmostreecxx::GKeyFile &kf)
{
  ::rust::MaybeUninit< ::rust::Box< ::rpmostreecxx::Treefile> > return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$origin_to_treefile (kf, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

::rpmostreecxx::GKeyFile *
treefile_to_origin (const ::rpmostreecxx::Treefile &tf)
{
  ::rust::MaybeUninit< ::rpmostreecxx::GKeyFile *> return$;
  ::rust::repr::PtrLen error$ = rpmostreecxx$cxxbridge1$treefile_to_origin (tf, &return$.value);
  if (error$.ptr)
    {
      throw ::rust::impl< ::rust::Error>::error (error$);
    }
  return ::std::move (return$.value);
}

void
origin_validate_roundtrip (const ::rpmostreecxx::GKeyFile &kf) noexcept
{
  rpmostreecxx$cxxbridge1$origin_validate_roundtrip (kf);
}

::rust::String
cache_branch_to_nevra (::rust::Str nevra) noexcept
{
  ::rust::MaybeUninit< ::rust::String> return$;
  rpmostreecxx$cxxbridge1$cache_branch_to_nevra (nevra, &return$.value);
  return ::std::move (return$.value);
}
} // namespace rpmostreecxx

extern "C"
{
  ::rpmostreecxx::Bubblewrap *cxxbridge1$box$rpmostreecxx$Bubblewrap$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$Bubblewrap$dealloc (::rpmostreecxx::Bubblewrap *) noexcept;
  void cxxbridge1$box$rpmostreecxx$Bubblewrap$drop (
      ::rust::Box< ::rpmostreecxx::Bubblewrap> *ptr) noexcept;

  ::rpmostreecxx::ContainerImageState *
  cxxbridge1$box$rpmostreecxx$ContainerImageState$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$ContainerImageState$dealloc (
      ::rpmostreecxx::ContainerImageState *) noexcept;
  void cxxbridge1$box$rpmostreecxx$ContainerImageState$drop (
      ::rust::Box< ::rpmostreecxx::ContainerImageState> *ptr) noexcept;

  ::rpmostreecxx::TempEtcGuard *cxxbridge1$box$rpmostreecxx$TempEtcGuard$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$TempEtcGuard$dealloc (::rpmostreecxx::TempEtcGuard *) noexcept;
  void cxxbridge1$box$rpmostreecxx$TempEtcGuard$drop (
      ::rust::Box< ::rpmostreecxx::TempEtcGuard> *ptr) noexcept;

  ::rpmostreecxx::FilesystemScriptPrep *
  cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$dealloc (
      ::rpmostreecxx::FilesystemScriptPrep *) noexcept;
  void cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$drop (
      ::rust::Box< ::rpmostreecxx::FilesystemScriptPrep> *ptr) noexcept;

  ::rpmostreecxx::RpmImporterFlags *cxxbridge1$box$rpmostreecxx$RpmImporterFlags$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$RpmImporterFlags$dealloc (
      ::rpmostreecxx::RpmImporterFlags *) noexcept;
  void cxxbridge1$box$rpmostreecxx$RpmImporterFlags$drop (
      ::rust::Box< ::rpmostreecxx::RpmImporterFlags> *ptr) noexcept;

  ::rpmostreecxx::RpmImporter *cxxbridge1$box$rpmostreecxx$RpmImporter$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$RpmImporter$dealloc (::rpmostreecxx::RpmImporter *) noexcept;
  void cxxbridge1$box$rpmostreecxx$RpmImporter$drop (
      ::rust::Box< ::rpmostreecxx::RpmImporter> *ptr) noexcept;

  ::rpmostreecxx::HistoryCtx *cxxbridge1$box$rpmostreecxx$HistoryCtx$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$HistoryCtx$dealloc (::rpmostreecxx::HistoryCtx *) noexcept;
  void cxxbridge1$box$rpmostreecxx$HistoryCtx$drop (
      ::rust::Box< ::rpmostreecxx::HistoryCtx> *ptr) noexcept;

  ::rpmostreecxx::TokioHandle *cxxbridge1$box$rpmostreecxx$TokioHandle$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$TokioHandle$dealloc (::rpmostreecxx::TokioHandle *) noexcept;
  void cxxbridge1$box$rpmostreecxx$TokioHandle$drop (
      ::rust::Box< ::rpmostreecxx::TokioHandle> *ptr) noexcept;

  ::rpmostreecxx::TokioEnterGuard *cxxbridge1$box$rpmostreecxx$TokioEnterGuard$alloc () noexcept;
  void
  cxxbridge1$box$rpmostreecxx$TokioEnterGuard$dealloc (::rpmostreecxx::TokioEnterGuard *) noexcept;
  void cxxbridge1$box$rpmostreecxx$TokioEnterGuard$drop (
      ::rust::Box< ::rpmostreecxx::TokioEnterGuard> *ptr) noexcept;

  ::rpmostreecxx::Treefile *cxxbridge1$box$rpmostreecxx$Treefile$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$Treefile$dealloc (::rpmostreecxx::Treefile *) noexcept;
  void
  cxxbridge1$box$rpmostreecxx$Treefile$drop (::rust::Box< ::rpmostreecxx::Treefile> *ptr) noexcept;

  void cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$new (
      const ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$drop (
      ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$len (
      const ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$capacity (
      const ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr) noexcept;
  const ::rpmostreecxx::OverrideReplacement *
  cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$data (
      const ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$reserve_total (
      ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr, ::std::size_t new_cap) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$set_len (
      ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr, ::std::size_t len) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$truncate (
      ::rust::Vec< ::rpmostreecxx::OverrideReplacement> *ptr, ::std::size_t len) noexcept;

  void cxxbridge1$rust_vec$rpmostreecxx$StringMapping$new (
      const ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$StringMapping$drop (
      ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$StringMapping$len (
      const ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$StringMapping$capacity (
      const ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr) noexcept;
  const ::rpmostreecxx::StringMapping *cxxbridge1$rust_vec$rpmostreecxx$StringMapping$data (
      const ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$StringMapping$reserve_total (
      ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr, ::std::size_t new_cap) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$StringMapping$set_len (
      ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr, ::std::size_t len) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$StringMapping$truncate (
      ::rust::Vec< ::rpmostreecxx::StringMapping> *ptr, ::std::size_t len) noexcept;

  ::rpmostreecxx::PasswdDB *cxxbridge1$box$rpmostreecxx$PasswdDB$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$PasswdDB$dealloc (::rpmostreecxx::PasswdDB *) noexcept;
  void
  cxxbridge1$box$rpmostreecxx$PasswdDB$drop (::rust::Box< ::rpmostreecxx::PasswdDB> *ptr) noexcept;

  ::rpmostreecxx::PasswdEntries *cxxbridge1$box$rpmostreecxx$PasswdEntries$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$PasswdEntries$dealloc (::rpmostreecxx::PasswdEntries *) noexcept;
  void cxxbridge1$box$rpmostreecxx$PasswdEntries$drop (
      ::rust::Box< ::rpmostreecxx::PasswdEntries> *ptr) noexcept;

  ::rpmostreecxx::Extensions *cxxbridge1$box$rpmostreecxx$Extensions$alloc () noexcept;
  void cxxbridge1$box$rpmostreecxx$Extensions$dealloc (::rpmostreecxx::Extensions *) noexcept;
  void cxxbridge1$box$rpmostreecxx$Extensions$drop (
      ::rust::Box< ::rpmostreecxx::Extensions> *ptr) noexcept;

  ::rpmostreecxx::LockfileConfig *cxxbridge1$box$rpmostreecxx$LockfileConfig$alloc () noexcept;
  void
  cxxbridge1$box$rpmostreecxx$LockfileConfig$dealloc (::rpmostreecxx::LockfileConfig *) noexcept;
  void cxxbridge1$box$rpmostreecxx$LockfileConfig$drop (
      ::rust::Box< ::rpmostreecxx::LockfileConfig> *ptr) noexcept;

  void cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$new (
      const ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$drop (
      ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$len (
      const ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr) noexcept;
  ::std::size_t cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$capacity (
      const ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr) noexcept;
  const ::rpmostreecxx::LockedPackage *cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$data (
      const ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$reserve_total (
      ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr, ::std::size_t new_cap) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$set_len (
      ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr, ::std::size_t len) noexcept;
  void cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$truncate (
      ::rust::Vec< ::rpmostreecxx::LockedPackage> *ptr, ::std::size_t len) noexcept;

  static_assert (::rust::detail::is_complete< ::rpmostreecxx::ClientConnection>::value,
                 "definition of ClientConnection is required");
  static_assert (sizeof (::std::unique_ptr< ::rpmostreecxx::ClientConnection>) == sizeof (void *),
                 "");
  static_assert (alignof (::std::unique_ptr< ::rpmostreecxx::ClientConnection>) == alignof (void *),
                 "");
  void
  cxxbridge1$unique_ptr$rpmostreecxx$ClientConnection$null (
      ::std::unique_ptr< ::rpmostreecxx::ClientConnection> *ptr) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::ClientConnection> ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$ClientConnection$raw (
      ::std::unique_ptr< ::rpmostreecxx::ClientConnection> *ptr,
      ::rpmostreecxx::ClientConnection *raw) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::ClientConnection> (raw);
  }
  const ::rpmostreecxx::ClientConnection *
  cxxbridge1$unique_ptr$rpmostreecxx$ClientConnection$get (
      const ::std::unique_ptr< ::rpmostreecxx::ClientConnection> &ptr) noexcept
  {
    return ptr.get ();
  }
  ::rpmostreecxx::ClientConnection *
  cxxbridge1$unique_ptr$rpmostreecxx$ClientConnection$release (
      ::std::unique_ptr< ::rpmostreecxx::ClientConnection> &ptr) noexcept
  {
    return ptr.release ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$ClientConnection$drop (
      ::std::unique_ptr< ::rpmostreecxx::ClientConnection> *ptr) noexcept
  {
    ::rust::deleter_if< ::rust::detail::is_complete< ::rpmostreecxx::ClientConnection>::value>{}(
        ptr);
  }

  static_assert (::rust::detail::is_complete< ::rpmostreecxx::RPMDiff>::value,
                 "definition of RPMDiff is required");
  static_assert (sizeof (::std::unique_ptr< ::rpmostreecxx::RPMDiff>) == sizeof (void *), "");
  static_assert (alignof (::std::unique_ptr< ::rpmostreecxx::RPMDiff>) == alignof (void *), "");
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RPMDiff$null (
      ::std::unique_ptr< ::rpmostreecxx::RPMDiff> *ptr) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::RPMDiff> ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RPMDiff$raw (::std::unique_ptr< ::rpmostreecxx::RPMDiff> *ptr,
                                                  ::rpmostreecxx::RPMDiff *raw) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::RPMDiff> (raw);
  }
  const ::rpmostreecxx::RPMDiff *
  cxxbridge1$unique_ptr$rpmostreecxx$RPMDiff$get (
      const ::std::unique_ptr< ::rpmostreecxx::RPMDiff> &ptr) noexcept
  {
    return ptr.get ();
  }
  ::rpmostreecxx::RPMDiff *
  cxxbridge1$unique_ptr$rpmostreecxx$RPMDiff$release (
      ::std::unique_ptr< ::rpmostreecxx::RPMDiff> &ptr) noexcept
  {
    return ptr.release ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RPMDiff$drop (
      ::std::unique_ptr< ::rpmostreecxx::RPMDiff> *ptr) noexcept
  {
    ::rust::deleter_if< ::rust::detail::is_complete< ::rpmostreecxx::RPMDiff>::value>{}(ptr);
  }

  static_assert (::rust::detail::is_complete< ::rpmostreecxx::Progress>::value,
                 "definition of Progress is required");
  static_assert (sizeof (::std::unique_ptr< ::rpmostreecxx::Progress>) == sizeof (void *), "");
  static_assert (alignof (::std::unique_ptr< ::rpmostreecxx::Progress>) == alignof (void *), "");
  void
  cxxbridge1$unique_ptr$rpmostreecxx$Progress$null (
      ::std::unique_ptr< ::rpmostreecxx::Progress> *ptr) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::Progress> ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$Progress$raw (
      ::std::unique_ptr< ::rpmostreecxx::Progress> *ptr, ::rpmostreecxx::Progress *raw) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::Progress> (raw);
  }
  const ::rpmostreecxx::Progress *
  cxxbridge1$unique_ptr$rpmostreecxx$Progress$get (
      const ::std::unique_ptr< ::rpmostreecxx::Progress> &ptr) noexcept
  {
    return ptr.get ();
  }
  ::rpmostreecxx::Progress *
  cxxbridge1$unique_ptr$rpmostreecxx$Progress$release (
      ::std::unique_ptr< ::rpmostreecxx::Progress> &ptr) noexcept
  {
    return ptr.release ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$Progress$drop (
      ::std::unique_ptr< ::rpmostreecxx::Progress> *ptr) noexcept
  {
    ::rust::deleter_if< ::rust::detail::is_complete< ::rpmostreecxx::Progress>::value>{}(ptr);
  }

  static_assert (::rust::detail::is_complete< ::rpmostreecxx::RpmTs>::value,
                 "definition of RpmTs is required");
  static_assert (sizeof (::std::unique_ptr< ::rpmostreecxx::RpmTs>) == sizeof (void *), "");
  static_assert (alignof (::std::unique_ptr< ::rpmostreecxx::RpmTs>) == alignof (void *), "");
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RpmTs$null (
      ::std::unique_ptr< ::rpmostreecxx::RpmTs> *ptr) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::RpmTs> ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RpmTs$raw (::std::unique_ptr< ::rpmostreecxx::RpmTs> *ptr,
                                                ::rpmostreecxx::RpmTs *raw) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::RpmTs> (raw);
  }
  const ::rpmostreecxx::RpmTs *
  cxxbridge1$unique_ptr$rpmostreecxx$RpmTs$get (
      const ::std::unique_ptr< ::rpmostreecxx::RpmTs> &ptr) noexcept
  {
    return ptr.get ();
  }
  ::rpmostreecxx::RpmTs *
  cxxbridge1$unique_ptr$rpmostreecxx$RpmTs$release (
      ::std::unique_ptr< ::rpmostreecxx::RpmTs> &ptr) noexcept
  {
    return ptr.release ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$RpmTs$drop (
      ::std::unique_ptr< ::rpmostreecxx::RpmTs> *ptr) noexcept
  {
    ::rust::deleter_if< ::rust::detail::is_complete< ::rpmostreecxx::RpmTs>::value>{}(ptr);
  }

  static_assert (::rust::detail::is_complete< ::rpmostreecxx::PackageMeta>::value,
                 "definition of PackageMeta is required");
  static_assert (sizeof (::std::unique_ptr< ::rpmostreecxx::PackageMeta>) == sizeof (void *), "");
  static_assert (alignof (::std::unique_ptr< ::rpmostreecxx::PackageMeta>) == alignof (void *), "");
  void
  cxxbridge1$unique_ptr$rpmostreecxx$PackageMeta$null (
      ::std::unique_ptr< ::rpmostreecxx::PackageMeta> *ptr) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::PackageMeta> ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$PackageMeta$raw (
      ::std::unique_ptr< ::rpmostreecxx::PackageMeta> *ptr,
      ::rpmostreecxx::PackageMeta *raw) noexcept
  {
    ::new (ptr)::std::unique_ptr< ::rpmostreecxx::PackageMeta> (raw);
  }
  const ::rpmostreecxx::PackageMeta *
  cxxbridge1$unique_ptr$rpmostreecxx$PackageMeta$get (
      const ::std::unique_ptr< ::rpmostreecxx::PackageMeta> &ptr) noexcept
  {
    return ptr.get ();
  }
  ::rpmostreecxx::PackageMeta *
  cxxbridge1$unique_ptr$rpmostreecxx$PackageMeta$release (
      ::std::unique_ptr< ::rpmostreecxx::PackageMeta> &ptr) noexcept
  {
    return ptr.release ();
  }
  void
  cxxbridge1$unique_ptr$rpmostreecxx$PackageMeta$drop (
      ::std::unique_ptr< ::rpmostreecxx::PackageMeta> *ptr) noexcept
  {
    ::rust::deleter_if< ::rust::detail::is_complete< ::rpmostreecxx::PackageMeta>::value>{}(ptr);
  }
} // extern "C"

namespace rust
{
inline namespace cxxbridge1
{
template <>
::rpmostreecxx::Bubblewrap *
Box< ::rpmostreecxx::Bubblewrap>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$Bubblewrap$alloc ();
}
template <>
void
Box< ::rpmostreecxx::Bubblewrap>::allocation::dealloc (::rpmostreecxx::Bubblewrap *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$Bubblewrap$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::Bubblewrap>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$Bubblewrap$drop (this);
}
template <>
::rpmostreecxx::ContainerImageState *
Box< ::rpmostreecxx::ContainerImageState>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$ContainerImageState$alloc ();
}
template <>
void
Box< ::rpmostreecxx::ContainerImageState>::allocation::dealloc (
    ::rpmostreecxx::ContainerImageState *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$ContainerImageState$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::ContainerImageState>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$ContainerImageState$drop (this);
}
template <>
::rpmostreecxx::TempEtcGuard *
Box< ::rpmostreecxx::TempEtcGuard>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$TempEtcGuard$alloc ();
}
template <>
void
Box< ::rpmostreecxx::TempEtcGuard>::allocation::dealloc (::rpmostreecxx::TempEtcGuard *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$TempEtcGuard$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::TempEtcGuard>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$TempEtcGuard$drop (this);
}
template <>
::rpmostreecxx::FilesystemScriptPrep *
Box< ::rpmostreecxx::FilesystemScriptPrep>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$alloc ();
}
template <>
void
Box< ::rpmostreecxx::FilesystemScriptPrep>::allocation::dealloc (
    ::rpmostreecxx::FilesystemScriptPrep *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::FilesystemScriptPrep>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$FilesystemScriptPrep$drop (this);
}
template <>
::rpmostreecxx::RpmImporterFlags *
Box< ::rpmostreecxx::RpmImporterFlags>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$RpmImporterFlags$alloc ();
}
template <>
void
Box< ::rpmostreecxx::RpmImporterFlags>::allocation::dealloc (
    ::rpmostreecxx::RpmImporterFlags *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$RpmImporterFlags$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::RpmImporterFlags>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$RpmImporterFlags$drop (this);
}
template <>
::rpmostreecxx::RpmImporter *
Box< ::rpmostreecxx::RpmImporter>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$RpmImporter$alloc ();
}
template <>
void
Box< ::rpmostreecxx::RpmImporter>::allocation::dealloc (::rpmostreecxx::RpmImporter *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$RpmImporter$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::RpmImporter>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$RpmImporter$drop (this);
}
template <>
::rpmostreecxx::HistoryCtx *
Box< ::rpmostreecxx::HistoryCtx>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$HistoryCtx$alloc ();
}
template <>
void
Box< ::rpmostreecxx::HistoryCtx>::allocation::dealloc (::rpmostreecxx::HistoryCtx *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$HistoryCtx$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::HistoryCtx>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$HistoryCtx$drop (this);
}
template <>
::rpmostreecxx::TokioHandle *
Box< ::rpmostreecxx::TokioHandle>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$TokioHandle$alloc ();
}
template <>
void
Box< ::rpmostreecxx::TokioHandle>::allocation::dealloc (::rpmostreecxx::TokioHandle *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$TokioHandle$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::TokioHandle>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$TokioHandle$drop (this);
}
template <>
::rpmostreecxx::TokioEnterGuard *
Box< ::rpmostreecxx::TokioEnterGuard>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$TokioEnterGuard$alloc ();
}
template <>
void
Box< ::rpmostreecxx::TokioEnterGuard>::allocation::dealloc (
    ::rpmostreecxx::TokioEnterGuard *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$TokioEnterGuard$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::TokioEnterGuard>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$TokioEnterGuard$drop (this);
}
template <>
::rpmostreecxx::Treefile *
Box< ::rpmostreecxx::Treefile>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$Treefile$alloc ();
}
template <>
void
Box< ::rpmostreecxx::Treefile>::allocation::dealloc (::rpmostreecxx::Treefile *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$Treefile$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::Treefile>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$Treefile$drop (this);
}
template <> Vec< ::rpmostreecxx::OverrideReplacement>::Vec () noexcept
{
  cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$new (this);
}
template <>
void
Vec< ::rpmostreecxx::OverrideReplacement>::drop () noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$drop (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::OverrideReplacement>::size () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$len (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::OverrideReplacement>::capacity () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$capacity (this);
}
template <>
const ::rpmostreecxx::OverrideReplacement *
Vec< ::rpmostreecxx::OverrideReplacement>::data () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$data (this);
}
template <>
void
Vec< ::rpmostreecxx::OverrideReplacement>::reserve_total (::std::size_t new_cap) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$reserve_total (this, new_cap);
}
template <>
void
Vec< ::rpmostreecxx::OverrideReplacement>::set_len (::std::size_t len) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$set_len (this, len);
}
template <>
void
Vec< ::rpmostreecxx::OverrideReplacement>::truncate (::std::size_t len)
{
  return cxxbridge1$rust_vec$rpmostreecxx$OverrideReplacement$truncate (this, len);
}
template <> Vec< ::rpmostreecxx::StringMapping>::Vec () noexcept
{
  cxxbridge1$rust_vec$rpmostreecxx$StringMapping$new (this);
}
template <>
void
Vec< ::rpmostreecxx::StringMapping>::drop () noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$drop (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::StringMapping>::size () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$len (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::StringMapping>::capacity () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$capacity (this);
}
template <>
const ::rpmostreecxx::StringMapping *
Vec< ::rpmostreecxx::StringMapping>::data () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$data (this);
}
template <>
void
Vec< ::rpmostreecxx::StringMapping>::reserve_total (::std::size_t new_cap) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$reserve_total (this, new_cap);
}
template <>
void
Vec< ::rpmostreecxx::StringMapping>::set_len (::std::size_t len) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$set_len (this, len);
}
template <>
void
Vec< ::rpmostreecxx::StringMapping>::truncate (::std::size_t len)
{
  return cxxbridge1$rust_vec$rpmostreecxx$StringMapping$truncate (this, len);
}
template <>
::rpmostreecxx::PasswdDB *
Box< ::rpmostreecxx::PasswdDB>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$PasswdDB$alloc ();
}
template <>
void
Box< ::rpmostreecxx::PasswdDB>::allocation::dealloc (::rpmostreecxx::PasswdDB *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$PasswdDB$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::PasswdDB>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$PasswdDB$drop (this);
}
template <>
::rpmostreecxx::PasswdEntries *
Box< ::rpmostreecxx::PasswdEntries>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$PasswdEntries$alloc ();
}
template <>
void
Box< ::rpmostreecxx::PasswdEntries>::allocation::dealloc (
    ::rpmostreecxx::PasswdEntries *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$PasswdEntries$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::PasswdEntries>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$PasswdEntries$drop (this);
}
template <>
::rpmostreecxx::Extensions *
Box< ::rpmostreecxx::Extensions>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$Extensions$alloc ();
}
template <>
void
Box< ::rpmostreecxx::Extensions>::allocation::dealloc (::rpmostreecxx::Extensions *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$Extensions$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::Extensions>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$Extensions$drop (this);
}
template <>
::rpmostreecxx::LockfileConfig *
Box< ::rpmostreecxx::LockfileConfig>::allocation::alloc () noexcept
{
  return cxxbridge1$box$rpmostreecxx$LockfileConfig$alloc ();
}
template <>
void
Box< ::rpmostreecxx::LockfileConfig>::allocation::dealloc (
    ::rpmostreecxx::LockfileConfig *ptr) noexcept
{
  cxxbridge1$box$rpmostreecxx$LockfileConfig$dealloc (ptr);
}
template <>
void
Box< ::rpmostreecxx::LockfileConfig>::drop () noexcept
{
  cxxbridge1$box$rpmostreecxx$LockfileConfig$drop (this);
}
template <> Vec< ::rpmostreecxx::LockedPackage>::Vec () noexcept
{
  cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$new (this);
}
template <>
void
Vec< ::rpmostreecxx::LockedPackage>::drop () noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$drop (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::LockedPackage>::size () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$len (this);
}
template <>
::std::size_t
Vec< ::rpmostreecxx::LockedPackage>::capacity () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$capacity (this);
}
template <>
const ::rpmostreecxx::LockedPackage *
Vec< ::rpmostreecxx::LockedPackage>::data () const noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$data (this);
}
template <>
void
Vec< ::rpmostreecxx::LockedPackage>::reserve_total (::std::size_t new_cap) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$reserve_total (this, new_cap);
}
template <>
void
Vec< ::rpmostreecxx::LockedPackage>::set_len (::std::size_t len) noexcept
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$set_len (this, len);
}
template <>
void
Vec< ::rpmostreecxx::LockedPackage>::truncate (::std::size_t len)
{
  return cxxbridge1$rust_vec$rpmostreecxx$LockedPackage$truncate (this, len);
}
} // namespace cxxbridge1
} // namespace rust
