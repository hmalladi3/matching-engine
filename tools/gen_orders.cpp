// Writes a seeded, reproducible request stream in the assignment's CSV format.
//
// Usage: gen_orders --profile tight|deep|cancel_heavy|sweep|id_reuse|extreme|mixed
//                   --count N [--seed S]
// The same arguments produce byte-identical output on every platform.
// @spec DLV-TEST-001
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

#include "support/request_generator.h"

int main(int argc, char** argv) {
    using namespace matcher;
    std::string_view profile_name = "mixed";
    unsigned long long count = 1'000'000, seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (flag == "--profile") profile_name = argv[i + 1];
        else if (flag == "--count") count = std::stoull(argv[i + 1]);
        else if (flag == "--seed") seed = std::stoull(argv[i + 1]);
        else argc = 0;  // force usage
    }
    const auto profile = test::profile_from_name(profile_name);
    if (argc % 2 == 0 || !profile) {
        std::fprintf(stderr,
                     "usage: gen_orders --profile tight|deep|cancel_heavy|sweep|id_reuse|extreme|mixed "
                     "--count N [--seed S]\n");
        return 2;
    }

    test::RequestGenerator generator(*profile, seed);
    char price[Price::kMaxFormattedLen + 1];
    for (unsigned long long i = 0; i < count; ++i) {
        const test::Request request = generator.next();
        if (const auto* a = std::get_if<AddOrder>(&request)) {
            *format_price(a->price, price) = '\0';
            std::printf("0,%llu,%d,%llu,%s\n", static_cast<unsigned long long>(a->id), static_cast<int>(a->side),
                        static_cast<unsigned long long>(a->qty), price);
        } else {
            std::printf("1,%llu\n", static_cast<unsigned long long>(std::get<CancelOrder>(request).id));
        }
    }
    return 0;
}
