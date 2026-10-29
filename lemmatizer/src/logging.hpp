#pragma once
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <chrono>
#include <iostream>
#include <string>
namespace lemmatizer {
inline void log_event(const std::string& event, const std::string& run_id,
                      const std::string& message = {}, std::int64_t records = 0) {
    using bsoncxx::builder::basic::kvp;
    auto entry = bsoncxx::builder::basic::make_document(kvp("event", event), kvp("run_id", run_id),
        kvp("message", message), kvp("records", records),
        kvp("time", bsoncxx::types::b_date(std::chrono::system_clock::now())));
    std::cerr << bsoncxx::to_json(entry.view(), bsoncxx::ExtendedJsonMode::k_relaxed) << '\n';
}
}
