#include "protoscope/storage/store.hpp"
#include "test_helpers.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    using namespace protoscope;
    using tests::require;
    int seconds = 10;
    if (argc > 2) return 2;
    if (argc == 2) {
        const std::string_view text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            seconds < 1 || seconds > 86400) return 2;
    }
    try {
        const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-storage-rate"));
        storage::Store store(directory.path(), "rate-test", {
            {"sample", {{"sequence", data::FieldType::Int64, false},
                        {"value", data::FieldType::Double, false}}}
        });
        store.start();
        store.waitIdle();
        require(store.status().recording, "record start failed");
        const auto begin = std::chrono::steady_clock::now();
        std::int64_t sent = 0;
        // 固定速率发布，不以数据库处理速度或 UI 帧率决定采样数量。
        for (int batch = 0; batch < seconds * 10; ++batch) {
            std::this_thread::sleep_until(begin + std::chrono::milliseconds(batch * 100));
            std::vector<data::Record> records;
            for (int index = 0; index < 100; ++index) {
                data::Record row;
                row.protocol = "rate-test"; row.dataset = "sample"; row.device = "device-a";
                row.receivedAtUs = sent * 1000;
                row.values = {{sent}, {static_cast<double>(sent) / 10.0}};
                records.push_back(std::move(row));
                ++sent;
            }
            std::string error;
            require(store.publish(std::move(records), error), error.c_str());
            require(!store.status().faulted, "record fault during rate test");
            store.poll();
        }
        store.stop();
        store.waitIdle();
        const auto status = store.status();
        require(status.received == static_cast<std::uint64_t>(sent) && status.queued == status.received &&
                status.committed == status.received && status.failed == 0, "record counters disagree");
        store.poll();
        std::int64_t read = 0;
        storage::Query query;
        query.dataset = "sample"; query.limit = 1000;
        bool more;
        do {
            store.query(query);
            store.waitIdle();
            auto events = store.poll();
            require(events.size() == 1 && events[0].ok, "query failed");
            const auto& event = events[0];
            for (const auto& row : event.records) {
                require(std::get<std::int64_t>(row.values.at(0).value) == read, "lost or reordered record");
                ++read;
            }
            query.snapshot = event.snapshot;
            query.offset = static_cast<std::size_t>(read);
            more = event.more;
        } while (more);
        require(read == sent, "query count disagrees with committed count");
        std::cout << "rate=1000/s seconds=" << seconds << " received=" << status.received
                  << " queued=" << status.queued << " committed=" << status.committed
                  << " queried=" << read << " failed=" << status.failed << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
