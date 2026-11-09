#include "repository.hpp"
#include "app.hpp"
#include <mongocxx/instance.hpp>
#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc,char** argv) {
    using namespace boolean_search;
    app::signals();
    try {
        BuildOptions o;
        if (const auto* uri=std::getenv("MONGODB_URI")) o.uri=uri;
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--help") {
                std::cout << "Usage: boolean-index --output indexes/INDEX [--lemmatization-run ID]\n"
                    "  [--uri URI] [--database crawler] [--memory-budget-mib 256]\n"
                    "Input: latest completed lemmatization unless explicitly selected.\n"
                    "Builds a new immutable index; existing output is never replaced.\n";
                return 0;
            }
            if (i+1==argc) throw std::invalid_argument("Missing value for "+key);
            const std::string value=argv[++i];
            if (key=="--output") o.output=value;
            else if (key=="--uri") o.uri=value;
            else if (key=="--database") o.database=value;
            else if (key=="--lemmatization-run") o.lemmatization_run=value;
            else if (key=="--memory-budget-mib") {
                const auto mib=app::number(value);
                if (mib>std::numeric_limits<std::size_t>::max()/(1024*1024)) throw std::invalid_argument("Memory budget too large");
                o.memory_bytes=static_cast<std::size_t>(mib)*1024*1024;
            } else throw std::invalid_argument("Unknown option: "+key);
        }
        if (o.output.empty() || o.uri.empty() || o.database.empty()) throw std::invalid_argument("--output, URI and database must be nonempty");
        mongocxx::instance instance;
        build_from_mongo(o,app::check);
        std::cout << "Completed: " << o.output << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Index build failed: " << e.what() << '\n';
        return app::stopped ? 130 : 2;
    }
}
