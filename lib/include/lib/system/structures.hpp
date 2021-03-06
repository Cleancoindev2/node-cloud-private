/* Send blaming letters to @yrtimd */
#ifndef __STRUCTURES_HPP__
#define __STRUCTURES_HPP__
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "allocators.hpp"

/* Containers */

template <typename BufferType>
class FixedBufferIterator {
public:
  FixedBufferIterator& operator++() {
    ptr_ = fcb_->incrementPtr(ptr_);
    circ_ = true;
    return *this;
  }

  bool operator!=(const FixedBufferIterator& rhs) {
    return ptr_ != rhs.ptr_ || circ_ != rhs.circ_;
  }

  typename BufferType::value_type& operator*() { return *ptr_; }
  typename BufferType::value_type* operator->() { return ptr_; }

private:
  typename BufferType::value_type* ptr_ = nullptr;
  bool circ_ = false;
  BufferType const* fcb_;

  friend BufferType;
};

template <typename T, uint32_t Size>
class FixedCircularBuffer {
public:
  using value_type = T;
  using const_iterator = FixedBufferIterator<FixedCircularBuffer>;

  FixedCircularBuffer():
    elements_(static_cast<T*>(malloc(sizeof(T) * Size))) {
  }

  ~FixedCircularBuffer() {
    clear();
    free(elements_);
  }

  FixedCircularBuffer(const FixedCircularBuffer&) = delete;
  FixedCircularBuffer(FixedCircularBuffer&& rhs): elements_(rhs.elements_),
                                                  head_(rhs.head_),
                                                  tail_(rhs.tail_),
                                                  size_(rhs.size_) {
    rhs.size_ = 0;
    rhs.elements_ = rhs.head_ = rhs.tail_ = nullptr;
  }

  FixedCircularBuffer& operator=(const FixedCircularBuffer&) = delete;
  FixedCircularBuffer& operator=(FixedCircularBuffer&&) = delete;

  template <typename... Args>
  T& emplace(Args&&... args) {
    T* place;
    if (size_ < Size) {
      place = tail_;
      tail_ = incrementPtr(tail_);
      ++size_;
    }
    else {
      head_->~T();
      place = head_;
      tail_ = head_ = incrementPtr(head_);
    }

    return *(new(place) T(std::forward<Args>(args)...));
  }

  const_iterator end() const {
    const_iterator ci;
    if (size_) ci.circ_ = true;
    ci.ptr_ = tail_;
    return ci;
  }

  const_iterator begin() const {
    const_iterator ci;
    ci.ptr_ = head_;
    ci.fcb_ = this;
    return ci;
  }

  T* frontPtr() const { return head_; }
  T* backPtr() const { return tail_; }

  void clear() {
    for (uint32_t i = size_; i > 0; --i) {
      head_->~T();
      head_ = incrementPtr(head_);
    }

    head_ = tail_ = elements_;
    size_ = 0;
  }

  void remove(T* toRem) {
    toRem->~T();
    --size_;

    if (toRem >= head_) {
      memmove(head_ + 1, head_, (toRem - head_) * sizeof(T));
      ++head_;
    }
    else {
      memmove(toRem, toRem + 1, (tail_ - toRem - 1) * sizeof(T));
      --tail_;
    }
  }

  uint32_t size() const { return size_; }

private:
  T* incrementPtr(T* ptr) const {
    if (++ptr == end_)
      ptr = elements_;

    return ptr;
  }

  T* elements_;

  T* head_ = elements_;
  T* tail_ = elements_;

  const T* end_ = elements_ + Size;

  uint32_t size_ = 0;
  friend const_iterator;
};

template <typename T, size_t Capacity>
class FixedVector {
public:
  FixedVector():
    elements_(static_cast<T*>(malloc(sizeof(T) * Capacity))),
    end_(elements_) { }

  ~FixedVector() {
    for (auto ptr = elements_; ptr != end_; ++ptr)
      ptr->~T();

    free(elements_);
  }

  FixedVector(const FixedVector&) = delete;
  FixedVector(FixedVector&& rhs): elements_(rhs.elements_),
                                  end_(rhs.end_) {
    rhs.elements_ = nullptr;
    rhs.end_ = nullptr;
  }

  FixedVector& operator=(const FixedVector&) = delete;
  FixedVector& operator=(FixedVector&&) = delete;

  template <typename... Args>
  T& emplace(Args&&... args) {
    return *(new(end_++) T(std::forward<Args>(args)...));
  }

  T* begin() const { return elements_; }
  T* end() const { return end_; }

  void remove(T* element) {
    element->~T();

    memmove(static_cast<void*>(element),
            static_cast<const void*>(element + 1),
            sizeof(T) * (end_ - element - 1));
    --end_;
  }

  uint32_t size() const { return end() - elements_; }

  bool contains(T* ptr) const {
    return begin() <= ptr && ptr < end();
  }

private:
  T* elements_;
  T* end_;
};

/* A simple queue-like counting hash-map of fixed size. Not
   thread-safe. */
template <typename ResultType, typename ArgType>
inline ResultType getHashIndex(const ArgType&);

template <typename KeyType,
          typename ArgType,
          typename IndexType = uint16_t,
          uint32_t MaxSize = 100000>
class FixedHashMap {
public:
  struct Element {
    Element *up, *down = nullptr;
    Element** bucket;

    KeyType key;
    ArgType data = {};

    ArgType& operator*() { return data; }

    Element(const KeyType& _key,
            Element** _bucket): bucket(_bucket),
                                key(_key) { }
  };

  FixedHashMap() {
    static_assert(MaxSize >= 2, "Your member is too small");

    const size_t bucketsSize = (1 << (sizeof(IndexType) * 8)) * sizeof(Element*);
    buckets_ = static_cast<Element**>(malloc(bucketsSize));
    memset(buckets_, 0, bucketsSize);
  }

  FixedHashMap(const FixedHashMap&) = delete;
  FixedHashMap(FixedHashMap&& rhs): buffer_(std::move(rhs.buffer_)),
                                    buckets_(rhs.buckets_) {
    rhs.buckets_ = nullptr;
  }

  ~FixedHashMap() {
    free(buckets_);
  }

  ArgType& tryStore(const KeyType& key) {
    Element** myBucket;
    auto foundElement = getElt(key, &myBucket);

    if (foundElement)
      return foundElement->data;

    // Element not found, add a new one
    if (buffer_.size() == MaxSize)
      preparePopLeft();

    Element& newComer = buffer_.emplace(key, myBucket);

    newComer.up = *myBucket;
    if (newComer.up) newComer.up->down = &newComer;
    *myBucket = &newComer;

    return newComer.data;
  }

  auto begin() { return buffer_.begin(); }
  auto end() { return buffer_.end(); }

private:
  Element* getElt(const KeyType& key, Element*** bucket) {
    const IndexType idx = getHashIndex<IndexType, KeyType>(key);
    *bucket = buckets_ + idx;

    Element* eltInBucket = **bucket;
    while (eltInBucket) {
      if (eltInBucket->key == key)
        return eltInBucket;

      eltInBucket = eltInBucket->up;
    }

    return nullptr;
  }

  void preparePopLeft() {
    auto toRemove = buffer_.frontPtr();

    if (toRemove->down) toRemove->down->up = toRemove->up;
    else *(toRemove->bucket) = toRemove->up;

    if (toRemove->up) toRemove->up->down = toRemove->down;
  }

  FixedCircularBuffer<Element, MaxSize> buffer_;
  Element** buckets_;
};

class CallsQueue {
public:
  static const uint32_t MAX_SIZE = 32;

  static CallsQueue& instance() {
    static CallsQueue inst;
    return inst;
  }

  // Called from a single thread
  inline void callAll();
  inline void insert(std::function<void()>);

private:
  CallsQueue() { }

  std::atomic<uint32_t> size_ = { 0 };
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::function<void()> calls_[MAX_SIZE];
};

inline void CallsQueue::callAll() {
  if (size_.load(std::memory_order_relaxed)) {
    SpinLock l(lock_);
    const auto end = calls_ + size_.load(std::memory_order_relaxed);

    for (auto ptr = calls_; ptr != end; ++ptr)
      (*ptr)();

    size_.store(0, std::memory_order_relaxed);
  }
}

inline void CallsQueue::insert(std::function<void()> f) {
  SpinLock l(lock_);
  auto sz = size_.load(std::memory_order_relaxed);
  if (sz >= MAX_SIZE) return;

  calls_[sz] = f;
  size_.fetch_add(1, std::memory_order_release);
}

template <size_t Length>
struct FixedString {
  FixedString() { }
  FixedString(const char* src) {
    memcpy(str, src, Length);
  }

  bool operator==(const FixedString& rhs) const {
    return memcmp(str, rhs.str, Length) == 0;
  }

  bool operator!=(const FixedString& rhs) const {
    return !(*this == rhs);
  }

  bool operator<(const FixedString& rhs) const {
    return memcmp(str, rhs.str, Length) < 0;
  }

  char str[Length];
};

template <uint32_t MaxSize>
class CharFunc {
public:
  CharFunc(uint32_t realSize) {
    const uint32_t bNum = realSize / 8;
    memset(bytes_, 0, (bNum + ((bNum * 8) != realSize)));
  }

  CharFunc(): CharFunc(MaxSize) { }

  bool checkPos(uint32_t id) const {
    uint32_t mask;
    const uint32_t& byte = getByte(id, mask);
    return byte & mask;
  }

  void setPos(uint32_t id, bool val) {
    uint32_t mask;
    uint32_t& byte = getByte(id, mask);

    if (val) byte|= mask;
    else byte&= ~mask;
  }

private:
  uint32_t& getByte(uint32_t id, uint32_t& mask) {
    const uint32_t oneElt = sizeof(uint32_t) * 8;
    uint32_t pos = id / oneElt;
    mask = 1 << (id - pos * oneElt);
    return bytes_[pos];
  }

  const uint32_t& getByte(uint32_t id, uint32_t& mask) const {
    return const_cast<CharFunc*>(this)->getByte(id, mask);
  }

  constexpr static uint32_t getMyBytesLength() {
    const uint32_t oneElt = sizeof(uint32_t) * 8;
    const uint32_t mS = MaxSize / oneElt;
    return mS + ((mS * oneElt) != MaxSize);
  }

  uint32_t bytes_[getMyBytesLength()];
};


#endif // __STRUCTURES_HPP__
