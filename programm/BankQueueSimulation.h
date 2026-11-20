#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bank_queue {

constexpr int SOURCE_COUNT = 3;
constexpr int OPERATION_COUNT = 4;
inline const std::array<std::string, SOURCE_COUNT> SOURCE_NAMES{
    "И1 (VIP)", "И2 (Премиум)", "И3 (Обычные)"
};
inline const std::array<std::string, OPERATION_COUNT> OPERATION_NAMES{
    "вклад", "кредит", "консультация", "обслуживание юрлиц"
};

struct ServiceInterval {
    double minimum = 5.0;
    double maximum = 15.0;
};

struct SimulationConfig {
    std::array<double, SOURCE_COUNT> rates{};
    int windows = 3;
    int bufferCapacity = 10;
    std::array<ServiceInterval, OPERATION_COUNT> service{};
    std::uint32_t seed = 0;
};

struct Request {
    int id = 0;
    int source = 0;
    int operation = 0;
    double arrival = 0.0;
    double serviceStart = 0.0;
    double serviceDuration = 0.0;
};

enum class EventType { ServiceEnd, Arrival };

struct Event {
    double time;
    EventType type;
    int index;
    std::uint64_t sequence;
};

struct EventLater {
    bool operator()(const Event& a, const Event& b) const {
        return std::tie(a.time, a.type, a.sequence) > std::tie(b.time, b.type, b.sequence);
    }
};

struct Device {
    std::optional<Request> request;
    double finishTime = 0.0;
    double busyTime = 0.0;
};

struct Moments {
    int count = 0;
    double mean = 0.0;
    double squaredDeviation = 0.0;

    void add(double value) {
        ++count;
        const double delta = value - mean;
        mean += delta / count;
        squaredDeviation += delta * (value - mean);
    }

    double variance() const { return squaredDeviation / count; }
};

struct SourceStats {
    int generated = 0;
    int served = 0;
    int rejected = 0;
    Moments wait;
    Moments service;
    Moments system;

    double rejectionProbability() const {
        return generated == 0 ? 0.0 : static_cast<double>(rejected) / generated;
    }
};

struct RunResult {
    std::array<SourceStats, SOURCE_COUNT> sources{};
    std::vector<Device> devices;
    double totalTime = 0.0;
    int totalGenerated = 0;
    int totalRejected = 0;

    double rejectionProbability() const {
        return totalGenerated == 0 ? 0.0 : static_cast<double>(totalRejected) / totalGenerated;
    }
};

inline std::string number(double value, int precision = 3) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

inline std::string column(const std::string& value, int width) {
    const auto length = std::count_if(value.begin(), value.end(), [](unsigned char character) {
        return (character & 0xC0) != 0x80;
    });
    const auto padding = length < width ? static_cast<std::size_t>(width - length) : 0;
    return value + std::string(padding, ' ') + " | ";
}

inline std::string sourceCode(int source) { return "И" + std::to_string(source + 1); }
inline std::string deviceCode(int device) { return "П" + std::to_string(device + 1); }

class Simulator {
public:
    Simulator(const SimulationConfig& config, int target, std::ostream* trace = nullptr,
              std::istream* pauses = nullptr)
        : config(config), target(target), arrivalRng(config.seed), serviceRng(config.seed ^ 0x9E3779B9u),
          trace(trace), pauses(pauses), buffer(config.bufferCapacity) {
        result.devices.resize(config.windows);
    }

    RunResult run() {
        for (int source = 0; source < SOURCE_COUNT; ++source) scheduleNextArrival(source);

        std::uint64_t step = 0;
        while (!calendar.empty()) {
            const Event event = calendar.top();
            calendar.pop();
            now = event.time;
            if (trace) {
                *trace << "\n" << std::string(72, '=') << "\nШАГ " << ++step
                       << " | МОДЕЛЬНОЕ ВРЕМЯ: " << number(now) << " мин\n";
            }

            if (event.type == EventType::Arrival) processArrival(event.index);
            else processServiceEnd(event.index);
            dispatch();

            if (trace) printState();
            if (pauses && !calendar.empty()) {
                *trace << "\nEnter — следующее событие...\n" << std::flush;
                std::string line;
                if (!std::getline(*pauses, line)) {
                    throw std::runtime_error("Ввод завершён. Пошаговый прогон прерван.");
                }
            }
        }

        result.totalTime = now;
        return std::move(result);
    }

private:
    SimulationConfig config;
    int target;
    std::mt19937 arrivalRng;
    std::mt19937 serviceRng;
    std::ostream* trace;
    std::istream* pauses;
    RunResult result;
    std::priority_queue<Event, std::vector<Event>, EventLater> calendar;
    std::vector<std::optional<Request>> buffer;
    std::deque<int> packagePending;
    int activePackageSource = -1;
    int activeInService = 0;
    int devicePointer = 0;
    double now = 0.0;
    std::uint64_t sequence = 0;

    static bool earlier(const Request& a, const Request& b) {
        return std::tie(a.arrival, a.id) < std::tie(b.arrival, b.id);
    }

    void addEvent(EventType type, int index, double time) {
        if (!std::isfinite(time)) {
            throw std::runtime_error("Время события вышло за числовой диапазон. Проверьте интервалы и интенсивности.");
        }
        calendar.push({time, type, index, ++sequence});
    }

    void scheduleNextArrival(int source) {
        std::exponential_distribution<double> interval(config.rates[source]);
        addEvent(EventType::Arrival, source, now + interval(arrivalRng));
    }

    int firstFreeSlot() const {
        for (int i = 0; i < config.bufferCapacity; ++i) {
            if (!buffer[i]) return i;
        }
        return -1;
    }

    int oldestSlot() const {
        int oldest = 0;
        for (int i = 1; i < config.bufferCapacity; ++i) {
            if (earlier(*buffer[i], *buffer[oldest])) oldest = i;
        }
        return oldest;
    }

    int nextFreeDevice() const {
        int device = devicePointer;
        for (int checked = 0; checked < config.windows; ++checked) {
            if (!result.devices[device].request) return device;
            device = (device + 1) % config.windows;
        }
        return -1;
    }

    void eraseFromPackage(int slot) {
        const auto position = std::find(packagePending.begin(), packagePending.end(), slot);
        if (position != packagePending.end()) packagePending.erase(position);
    }

    Request removeFromBuffer(int slot) {
        Request request = *buffer[slot];
        buffer[slot].reset();
        return request;
    }

    void discardPendingArrivals() {
        decltype(calendar) completions;
        while (!calendar.empty()) {
            if (calendar.top().type == EventType::ServiceEnd) completions.push(calendar.top());
            calendar.pop();
        }
        calendar.swap(completions);
    }

    void processArrival(int source) {
        const int id = ++result.totalGenerated;
        ++result.sources[source].generated;
        const int operation = std::uniform_int_distribution<int>(0, OPERATION_COUNT - 1)(serviceRng);
        const auto& interval = config.service[operation];
        const double duration = std::uniform_real_distribution<double>(interval.minimum, interval.maximum)(serviceRng);
        const Request request{id, source, operation, now, 0.0, duration};

        if (trace) {
            *trace << "ПРИХОД: " << SOURCE_NAMES[source] << ", талон " << id
                   << ", операция — " << OPERATION_NAMES[operation] << ".\n";
        }

        int slot = firstFreeSlot();
        if (slot == -1) {
            slot = oldestSlot();
            eraseFromPackage(slot);
            const Request evicted = removeFromBuffer(slot);
            auto& stats = result.sources[evicted.source];
            ++stats.rejected;
            ++result.totalRejected;
            stats.wait.add(now - evicted.arrival);
            stats.system.add(now - evicted.arrival);
            if (trace) {
                *trace << "ОТКАЗ Д1003: вытеснен талон " << evicted.id << " (" << sourceCode(evicted.source)
                       << ") с места " << slot + 1 << ".\n";
            }
        }
        buffer[slot] = request;
        if (trace) *trace << "БУФЕР Д1033: талон " << id << " занял место " << slot + 1 << ".\n";

        if (id == target) {
            discardPendingArrivals();
            if (trace) *trace << "Лимит приходов достигнут. Оставшиеся заявки продолжают обслуживаться.\n";
        } else {
            scheduleNextArrival(source);
        }
    }

    void processServiceEnd(int deviceId) {
        auto& device = result.devices[deviceId];
        const Request request = *device.request;
        device.request.reset();
        --activeInService;

        auto& stats = result.sources[request.source];
        ++stats.served;
        stats.wait.add(request.serviceStart - request.arrival);
        stats.service.add(request.serviceDuration);
        stats.system.add(now - request.arrival);
        if (trace) {
            *trace << "ОКОНЧАНИЕ: " << deviceCode(deviceId) << " обслужил талон " << request.id
                   << " (" << sourceCode(request.source) << ").\n";
        }
    }

    bool formPackage() {
        int source = SOURCE_COUNT;
        for (const auto& slot : buffer) {
            if (slot) source = std::min(source, slot->source);
        }
        if (source == SOURCE_COUNT) return false;

        activePackageSource = source;
        for (int i = 0; i < config.bufferCapacity; ++i) {
            if (buffer[i] && buffer[i]->source == source) packagePending.push_back(i);
        }
        std::sort(packagePending.begin(), packagePending.end(), [this](int a, int b) {
            return earlier(*buffer[a], *buffer[b]);
        });
        if (trace) {
            *trace << "ПАКЕТ Д2Б5: " << SOURCE_NAMES[source] << ", зафиксированы талоны";
            for (int slot : packagePending) *trace << ' ' << buffer[slot]->id;
            *trace << ".\n";
        }
        return true;
    }

    void startService(int deviceId, Request request) {
        request.serviceStart = now;
        auto& device = result.devices[deviceId];
        device.request = request;
        device.finishTime = now + request.serviceDuration;
        device.busyTime += request.serviceDuration;
        ++activeInService;
        addEvent(EventType::ServiceEnd, deviceId, device.finishTime);
        devicePointer = (deviceId + 1) % config.windows;
        if (trace) {
            *trace << "ВЫЗОВ Д2П2: талон " << request.id << " (" << sourceCode(request.source)
                   << ") → " << deviceCode(deviceId) << ", окончание в " << number(device.finishTime) << " мин.\n";
        }
    }

    void dispatch() {
        if (packagePending.empty() && activeInService == 0) {
            if (trace && activePackageSource != -1) *trace << "Активный пакет завершён.\n";
            activePackageSource = -1;
            if (!formPackage()) return;
        }
        while (!packagePending.empty()) {
            const int deviceId = nextFreeDevice();
            if (deviceId == -1) return;
            const int slot = packagePending.front();
            packagePending.pop_front();
            startService(deviceId, removeFromBuffer(slot));
        }
    }

    void printState() const {
        auto& out = *trace;
        out << "\nИСТОЧНИКИ\n" << column("Источник", 15) << column("Пришло", 11)
            << column("Обслужено", 11) << column("Отказов", 11) << "Отказы, %\n";
        for (int i = 0; i < SOURCE_COUNT; ++i) {
            const auto& stats = result.sources[i];
            out << column(SOURCE_NAMES[i], 15) << column(std::to_string(stats.generated), 11)
                << column(std::to_string(stats.served), 11) << column(std::to_string(stats.rejected), 11)
                << number(100.0 * stats.rejectionProbability(), 2) << '\n';
        }
        out << "Всего пришло: " << result.totalGenerated << '/' << target
            << "; отказов: " << result.totalRejected
            << " (" << number(100.0 * result.rejectionProbability(), 2) << "%)\n";

        out << "\nБУФЕР (" << config.bufferCapacity << " мест)\n" << column("Место", 8)
            << column("Талон", 11) << column("Источник", 12) << "Приход, мин\n";
        for (int i = 0; i < config.bufferCapacity; ++i) {
            const auto& slot = buffer[i];
            out << column(std::to_string(i + 1), 8);
            if (slot) {
                out << column(std::to_string(slot->id), 11) << column(sourceCode(slot->source), 12)
                    << number(slot->arrival) << '\n';
            } else {
                out << column("Свободно", 11) << column("—", 12) << "—\n";
            }
        }

        out << "\nОКНА\n" << column("Окно", 8) << column("Талон", 11)
            << column("Источник", 12) << "Окончание, мин\n";
        for (int i = 0; i < config.windows; ++i) {
            const auto& device = result.devices[i];
            out << column(deviceCode(i), 8);
            if (device.request) {
                out << column(std::to_string(device.request->id), 11)
                    << column(sourceCode(device.request->source), 12) << number(device.finishTime) << '\n';
            } else {
                out << column("Свободно", 11) << column("—", 12) << "—\n";
            }
        }

        out << "\nУказатель Д2П2: " << deviceCode(devicePointer) << "\nАктивный пакет: ";
        if (activePackageSource == -1) {
            out << "нет\n";
        } else {
            out << sourceCode(activePackageSource) << " | на обслуживании: " << activeInService
                << " | ожидают:";
            if (packagePending.empty()) out << " нет";
            for (int slot : packagePending) out << ' ' << buffer[slot]->id;
            out << '\n';
        }

        out << "\nКАЛЕНДАРЬ СОБЫТИЙ\n" << column("Время, мин", 14) << column("Событие", 14)
            << column("Источник", 10) << column("Окно", 8) << "Талон\n";
        if (calendar.empty()) out << "Календарь пуст.\n";
        auto events = calendar;
        while (!events.empty()) {
            const Event event = events.top();
            events.pop();
            out << column(number(event.time), 14);
            if (event.type == EventType::Arrival) {
                out << column("Приход", 14) << column(sourceCode(event.index), 10)
                    << column("—", 8) << "—\n";
            } else {
                const auto& request = *result.devices[event.index].request;
                out << column("Окончание", 14) << column(sourceCode(request.source), 10)
                    << column(deviceCode(event.index), 8) << request.id << '\n';
            }
        }
    }
};

}
