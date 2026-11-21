#include "test.hpp"

namespace ytest {
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
}  // namespace ytest

int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int failed = 0, run = 0;
    for (auto& c : ytest::registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        ++run;
        try {
            c.fn();
            std::cout << "  ok   " << c.name << "\n";
        } catch (const ytest::Failure& f) {
            ++failed;
            std::cout << "  FAIL " << c.name << "\n       " << f.msg << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "  FAIL " << c.name << "\n       exception: " << e.what() << "\n";
        }
    }
    std::cout << (run - failed) << "/" << run << " passed\n";
    return failed ? 1 : 0;
}
