#pragma once

#include "protoscope/data/model.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::storage {

struct Config {
    std::size_t queueBytes{32U * 1024U * 1024U};
    std::size_t batchRows{1000};
    std::chrono::milliseconds batchInterval{100};
    std::size_t kvValueBytes{256U * 1024U};
    std::size_t kvTotalBytes{8U * 1024U * 1024U};
    std::size_t kvDepth{16};
};

struct Status {
    bool recording{false};
    bool recovered{false};
    bool faulted{false};
    std::uint64_t received{0};
    std::uint64_t queued{0};
    std::uint64_t committed{0};
    std::uint64_t failed{0};
    std::size_t queueBytes{0};
    std::string error;
};

struct Query {
    std::string dataset;
    std::optional<std::string> device;
    std::optional<std::int64_t> fromUs;
    std::optional<std::int64_t> toUs;
    std::size_t offset{0};
    std::size_t limit{200};
    // 未指定时创建快照；返回的高水位用于后续页，后续发布不会改变同一快照。
    std::optional<std::int64_t> snapshot;
};

struct Completion {
    std::uint64_t task{0};
    std::string operation;
    bool ok{false};
    std::string error;
    std::vector<data::Record> records;
    std::optional<std::int64_t> snapshot;
    bool more{false};
    std::map<std::uint64_t, data::Schema> schemas;
};

class Store {
public:
    Store(std::filesystem::path root, std::string protocol, std::vector<data::Schema> schemas, Config config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::uint64_t start();
    std::uint64_t stop();
    bool publish(std::vector<data::Record> records, std::string& error);
    std::uint64_t query(Query query);
    void cancel(std::uint64_t task);
    std::optional<data::Value> get(const std::string& key) const;
    std::uint64_t set(std::string key, data::Value value);
    std::uint64_t erase(std::string key);
    std::uint64_t flush();
    Status status() const;
    std::vector<Completion> poll();
    void waitIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoscope::storage
