#pragma once
#include <utility>

template <typename T> class sk_sp {
public:
    sk_sp() : ptr(nullptr) {}
    sk_sp(T* p) : ptr(p) {}
    sk_sp(const sk_sp& that) : ptr(that.ptr) {}
    sk_sp& operator=(const sk_sp& that) { ptr = that.ptr; return *this; }
    template <typename U> sk_sp(const sk_sp<U>& that) : ptr(that.get()) {}
    template <typename U> sk_sp& operator=(const sk_sp<U>& that) { ptr = that.get(); return *this; }
    ~sk_sp() {}
    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
    void reset(T* p = nullptr) { ptr = p; }
private:
    T* ptr;
};

template <typename T, typename... Args>
sk_sp<T> sk_make_sp(Args&&... args) {
    return sk_sp<T>(new T(std::forward<Args>(args)...));
}

class SkRefCnt {
public:
    virtual ~SkRefCnt() = default;
};
