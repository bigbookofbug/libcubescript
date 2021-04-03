#ifndef LIBCUBESCRIPT_STD_HH
#define LIBCUBESCRIPT_STD_HH

#include <cubescript/cubescript.hh>

#include <cstddef>
#include <new>
#include <utility>
#include <type_traits>
#include <string_view>

#include "cs_state.hh"

namespace cubescript {

/* a value buffer */

template<typename T>
struct valbuf {
    valbuf() = delete;

    valbuf(internal_state *cs): buf{std_allocator<T>{cs}} {}

    using size_type = std::size_t;
    using value_type = T;
    using reference = T &;
    using const_reference = T const &;

    void reserve(std::size_t s) { buf.reserve(s); }
    void resize(std::size_t s) { buf.resize(s); }

    void resize(std::size_t s, value_type const &v) {
        buf.resize(s, v);
    }

    void append(T const *beg, T const *end) {
        buf.insert(buf.end(), beg, end);
    }

    void insert(std::size_t i, T const &it) {
        buf.insert(buf.begin() + i, it);
    }

    template<typename ...A>
    reference emplace_back(A &&...args) {
        return buf.emplace_back(std::forward<A>(args)...);
    }

    void push_back(T const &v) { buf.push_back(v); }
    void pop_back() { buf.pop_back(); }

    T &back() { return buf.back(); }
    T const &back() const { return buf.back(); }

    std::size_t size() const { return buf.size(); }
    std::size_t capacity() const { return buf.capacity(); }

    bool empty() const { return buf.empty(); }

    void clear() { buf.clear(); }

    T &operator[](std::size_t i) { return buf[i]; }
    T const &operator[](std::size_t i) const { return buf[i]; }

    T *data() { return &buf[0]; }
    T const *data() const { return &buf[0]; }

    std::vector<T, std_allocator<T>> buf;
};

/* specialization of value buffer for bytes */

struct charbuf: valbuf<char> {
    charbuf(internal_state *cs): valbuf<char>{cs} {}
    charbuf(state &cs);
    charbuf(thread_state &ts);

    void append(char const *beg, char const *end) {
        valbuf<char>::append(beg, end);
    }

    void append(std::string_view v) {
        append(&v[0], &v[v.size()]);
    }

    std::string_view str() {
        return std::string_view{buf.data(), buf.size()};
    }

    std::string_view str_term() {
        return std::string_view{buf.data(), buf.size() - 1};
    }
};

} /* namespace cubescript */

#endif
