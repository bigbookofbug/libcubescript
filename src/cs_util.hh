#ifndef LIBCUBESCRIPT_CS_UTIL_HH
#define LIBCUBESCRIPT_CS_UTIL_HH

#include <type_traits>
#include <unordered_map>
#include <vector>

namespace cscript {

cs_int cs_parse_int(
    std::string_view input, std::string_view *end = nullptr
);

cs_float cs_parse_float(
    std::string_view input, std::string_view *end = nullptr
);

template<typename F>
struct CsScopeExit {
    template<typename FF>
    CsScopeExit(FF &&f): func(std::forward<FF>(f)) {}
    ~CsScopeExit() {
        func();
    }
    std::decay_t<F> func;
};

template<typename F1, typename F2>
inline void cs_do_and_cleanup(F1 &&dof, F2 &&clf) {
    CsScopeExit<F2> cleanup(std::forward<F2>(clf));
    dof();
}

struct cs_strman;
struct cs_shared_state;

template<typename T>
struct cs_allocator {
    using value_type = T;

    cs_allocator(cs_shared_state *s): state{s} {}

    template<typename U>
    cs_allocator(cs_allocator<U> const &a): state{a.state} {};

    inline T *allocate(std::size_t n);

    inline void deallocate(T *p, std::size_t n);

    template<typename U>
    bool operator==(cs_allocator<U> const &a) {
        return state == a.state;
    }

    cs_shared_state *state;
};

template<typename T>
struct cs_valbuf {
    cs_valbuf() = delete;

    cs_valbuf(cs_shared_state &cs):
        buf{cs_allocator<T>{&cs}}
    {}

    cs_valbuf(cs_state &cs):
        buf{cs_allocator<T>{cs_get_sstate(cs)}}
    {}

    using size_type = std::size_t;
    using value_type = T;
    using reference = T &;
    using const_reference = T const &;

    void reserve(std::size_t s) { buf.reserve(s); }
    void resize(std::size_t s) { buf.resize(s); }

    void append(T const *beg, T const *end) {
        buf.insert(buf.end(), beg, end);
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

    std::vector<T, cs_allocator<T>> buf;
};

struct cs_charbuf: cs_valbuf<char> {
    cs_charbuf(cs_shared_state &cs): cs_valbuf<char>(cs) {}
    cs_charbuf(cs_state &cs): cs_valbuf<char>(cs) {}

    void append(char const *beg, char const *end) {
        cs_valbuf<char>::append(beg, end);
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

struct cs_shared_state {
    using allocator_type = cs_allocator<
        std::pair<std::string_view const, cs_ident *>
    >;
    cs_alloc_cb allocf;
    void *aptr;

    std::unordered_map<
        std::string_view, cs_ident *,
        std::hash<std::string_view>,
        std::equal_to<std::string_view>,
        allocator_type
    > idents;
    std::vector<cs_ident *, cs_allocator<cs_ident *>> identmap;

    cs_vprint_cb varprintf;
    cs_strman *strman;

    cs_shared_state() = delete;

    cs_shared_state(cs_alloc_cb af, void *data):
        allocf{af}, aptr{data},
        idents{allocator_type{this}},
        identmap{allocator_type{this}},
        varprintf{[](auto &, auto &) {}},
        strman{create<cs_strman>(this)}
    {}

    void *alloc(void *ptr, size_t os, size_t ns) {
        void *p = allocf(aptr, ptr, os, ns);
        if (!p && ns) {
            throw std::bad_alloc{};
        }
        return p;
    }

    template<typename T, typename ...A>
    T *create(A &&...args) {
        T *ret = static_cast<T *>(alloc(nullptr, 0, sizeof(T)));
        new (ret) T(std::forward<A>(args)...);
        return ret;
    }

    template<typename T>
    T *create_array(size_t len) {
        T *ret = static_cast<T *>(alloc(nullptr, 0, len * sizeof(T)));
        for (size_t i = 0; i < len; ++i) {
            new (&ret[i]) T();
        }
        return ret;
    }

    template<typename T>
    void destroy(T *v) noexcept {
        v->~T();
        alloc(v, sizeof(T), 0);
    }

    template<typename T>
    void destroy_array(T *v, size_t len) noexcept {
        v->~T();
        alloc(v, len * sizeof(T), 0);
    }
};

template<typename T>
inline T *cs_allocator<T>::allocate(std::size_t n) {
    return static_cast<T *>(state->alloc(nullptr, 0, n * sizeof(T)));
}

template<typename T>
inline void cs_allocator<T>::deallocate(T *p, std::size_t n) {
    state->alloc(p, n, 0);
}

inline cs_shared_state *cs_get_sstate(cs_state &cs) {
    return cs.p_state;
}

inline cs_strref cs_make_strref(char const *p, cs_shared_state &cs) {
    return cs_strref{p, cs};
}

/* string manager
 *
 * the purpose of this is to handle interning of strings; each string within
 * a libcs state is represented (and allocated) exactly once, and reference
 * counted; that both helps save resources, and potentially provide a means
 * to reliably represent returned strings in places that is compatible with
 * multiple threads and eliminate the chance of dangling pointers
 *
 * strings are allocated in a manner where the refcount and length are stored
 * as a part of the string's memory, so it can be easily accessed using just
 * the pointer to the string, but also this is transparent for usage
 *
 * this is not thread-safe yet, and later on it should be made that,
 * for now we don't bother...
 */

struct cs_strref_state {
    size_t length;
    size_t refcount;
};

struct cs_strman {
    using allocator_type = cs_allocator<
        std::pair<std::string_view const, cs_strref_state *>
    >;
    cs_strman() = delete;
    cs_strman(cs_shared_state *cs): cstate{cs}, counts{allocator_type{cs}} {}
    ~cs_strman() {}

    cs_strman(cs_strman const &) = delete;
    cs_strman(cs_strman &&) = delete;

    cs_strman &operator=(cs_strman const &) = delete;
    cs_strman &operator=(cs_strman &&) = delete;

    /* adds a string into the manager using any source, and returns a managed
     * version; this is "slow" as it has to hash the string and potentially
     * allocate fresh memory for it, but is perfectly safe at any time
     */
    char const *add(std::string_view str);

    /* this simply increments the reference count of an existing managed
     * string, this is only safe when you know the pointer you are passing
     * is already managed the system
     */
    char const *ref(char const *ptr);

    /* this will use the provided memory, assuming it is a fresh string that
     * is yet to be added; the memory must be allocated with alloc_buf()
     */
    char const *steal(char *ptr);

    /* decrements the reference count and removes it from the system if
     * that reaches zero; likewise, only safe with pointers that are managed
     */
    void unref(char const *ptr);

    /* just finds a managed pointer with the same contents
     * as the input, if not found then a null pointer is returned
     */
    char const *find(std::string_view str) const;

    /* a quick helper to make a proper string view out of a ptr */
    std::string_view get(char const *ptr) const;

    /* this will allocate a buffer of the given length (plus one for
     * terminating zero) so you can fill it; use steal() to write it
     */
    char *alloc_buf(std::size_t len) const;

    cs_shared_state *cstate;
    std::unordered_map<
        std::string_view, cs_strref_state *,
        std::hash<std::string_view>,
        std::equal_to<std::string_view>,
        allocator_type
    > counts;
};

} /* namespace cscript */

#endif /* LIBCUBESCRIPT_CS_UTIL_HH */
