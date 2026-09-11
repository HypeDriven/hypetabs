#include "../native/protocol.hpp"
#include <iostream>
#include <cstdlib>
using namespace hype::wire;
void check(bool ok, const char* name) { if (!ok) { std::cerr << name << '\n'; std::exit(1); } }
void invalid(std::string_view input) { try { Parser(input).parse(); check(false, "accepted invalid JSON"); } catch (const Error&) {} }
int main() {
    auto value = Parser(R"({"v":1,"type":"hello","label":"Work \uD83D\uDE00","ready":true})").parse();
    check(get_int(value, "v", 1, 1) == 1, "version");
    check(get_string(value, "label", 100) == "Work \xf0\x9f\x98\x80", "surrogate decoding");
    for (auto data : {"", "[]", "{}x", "{\"a\":1,}", "{\"a\":1,\"a\":2}", "{\"a\":01}", "{\"a\":1.0}", "{\"a\":9223372036854775808}", "{\"a\":null}", "{\"a\":[]}", "{\"a\":\"\\uD800\"}", "{\"a\":\"\\uDC00\"}", "{\"a\":\"\\x\"}"}) invalid(data);
    invalid("{\"a\":\"\xc0\x80\"}"); invalid("{\"a\":\"\xed\xa0\x80\"}");
    invalid(std::string(max_frame + 1, ' '));
    for (auto original : {std::string("quotes \" \\ \n\t"), std::string("\xf0\x9f\x98\x80"), std::string("ASCII")}) {
        auto roundtrip = Parser("{\"a\":" + quote(original) + "}").parse();
        check(get_string(roundtrip, "a", 100) == original, "roundtrip");
    }
    // Every truncated prefix of a valid message must be rejected.
    std::string complete = R"({"v":1,"type":"upsert","title":"x\u1234"})";
    for (size_t i = 0; i < complete.size(); ++i) invalid(complete.substr(0, i));
    std::cout << "Protocol checks passed\n";
}
