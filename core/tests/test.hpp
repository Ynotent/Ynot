// Tiny self-contained test harness (no external dependencies, builds anywhere).
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ytest {

struct Case {
    const char* name;
    std::function<void()> fn;
};
std::vector<Case>& registry();
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
struct Failure {
    std::string msg;
};

}  // namespace ytest

#define YT_CAT2(a, b) a##b
#define YT_CAT(a, b) YT_CAT2(a, b)
#define TEST(name)                                                              \
    static void YT_CAT(test_fn_, __LINE__)();                                   \
    static ytest::Registrar YT_CAT(test_reg_, __LINE__)(name, YT_CAT(test_fn_, __LINE__)); \
    static void YT_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::ostringstream os_;                                                        \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";              \
            throw ytest::Failure{os_.str()};                                               \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        auto va_ = (a);                                                                    \
        auto vb_ = (b);                                                                    \
        if (!(va_ == vb_)) {                                                               \
            std::ostringstream os_;                                                        \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed: "     \
                << va_ << " vs " << vb_;                                                   \
            throw ytest::Failure{os_.str()};                                               \
        }                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                              \
    do {                                                                                   \
        double va_ = (a), vb_ = (b);                                                       \
        if (std::fabs(va_ - vb_) > (eps)) {                                                \
            std::ostringstream os_;                                                        \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR failed: " << va_ << " vs " << vb_; \
            throw ytest::Failure{os_.str()};                                               \
        }                                                                                  \
    } while (0)

#define CHECK_THROWS(expr)                                                                 \
    do {                                                                                   \
        bool threw_ = false;                                                               \
        try { (void)(expr); } catch (...) { threw_ = true; }                               \
        if (!threw_) throw ytest::Failure{std::string(__FILE__) + ": expected throw: " #expr}; \
    } while (0)
