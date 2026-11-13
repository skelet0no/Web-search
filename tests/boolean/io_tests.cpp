#include "io.hpp"
#include "app.hpp"
#include "boolean/index.hpp"
#include "boolean/weighting.hpp"
#include "../support.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>

int main() {
    using namespace test_support;
    using namespace boolean_search;
    try {
        std::ostringstream out;
        io::put(out,0x12345678,4); io::put_string(out,"ёж");
        require(out.str().substr(0,4)==std::string("\x78\x56\x34\x12",4),"Fixed integer byte order");
        std::istringstream in(out.str());
        require(io::get(in,4)==0x12345678 && io::get_string(in)=="ёж","Fixed IO and Unicode string"); io::eof(in);
        rejects<std::runtime_error>([] { std::istringstream s("\0",std::ios::in); io::get(s); },"Truncated integer");
        rejects<std::runtime_error>([] { std::istringstream s("x"); io::eof(s); },"Trailing bytes");
        rejects<std::runtime_error>([] { std::ostringstream s; io::put_string(s,std::string(io::max_string+1,'x')); },"Oversize string");
        rejects<std::runtime_error>([] { std::ostringstream s; io::put(s,io::max_string+1,4); std::istringstream in(s.str()); io::get_string(in); },"Oversize stored length");
        for (const auto value:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            rejects<std::runtime_error>([&] { std::ostringstream s; io::put_double(s,value); },"Invalid norm written");
        rejects<std::runtime_error>([] {
            std::ostringstream s; io::put(s,UINT64_MAX); std::istringstream in(s.str()); io::get_double(in);
        },"NaN norm read");
        require(json_quote("\"\\\n\t")=="\"\\\"\\\\\\u000a\\u0009\"","JSON escaping");
        require(checked_frequency_sum(2,3)==5,"Frequency sum");
        rejects<std::overflow_error>([] { checked_frequency_sum(UINT64_MAX,1); },"Frequency overflow");
        require(inverse_document_frequency(10,10)==1 && term_weight(0,2)==0,"Weight boundary cases");
        rejects<std::runtime_error>([] { inverse_document_frequency(10,0); },"Zero DF");
        rejects<std::runtime_error>([] { inverse_document_frequency(10,11); },"DF exceeds N");
        require(app::number("0",true)==0 && app::number("42")==42,"CLI unsigned numbers");
        for (const auto* raw:{"","-1","1x","18446744073709551616"})
            rejects<std::invalid_argument>([&] { app::number(raw); },"Invalid CLI number");
        rejects<std::invalid_argument>([] { app::number("0"); },"Zero positive CLI number");
        rejects<std::runtime_error>([] { app::required({},"missing"); },"Missing metadata");
        std::cout << "Index IO unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
