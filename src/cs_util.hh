#ifndef LIBCUBESCRIPT_CS_UTIL_HH
#define LIBCUBESCRIPT_CS_UTIL_HH

#include <type_traits>
#include <unordered_map>

#include <ostd/string.hh>

namespace cscript {

template<typename K, typename V>
using CsMap = std::unordered_map<K, V>;

template<typename T>
using CsVector = std::vector<T>;

cs_int cs_parse_int(
    ostd::string_range input, ostd::string_range *end = nullptr
);

cs_float cs_parse_float(
    ostd::string_range input, ostd::string_range *end = nullptr
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

inline ostd::string_range slice_until(
    ostd::string_range s1, ostd::string_range s2
) {
    return s1.slice(0, &s2[0] - &s1[0]);
}

} /* namespace cscript */

#endif /* LIBCUBESCRIPT_CS_UTIL_HH */
