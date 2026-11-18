#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <vector>

using namespace std;

namespace {
constexpr int SOURCE_COUNT = 3;
constexpr int DEVICE_COUNT = 3;
constexpr int BUFFER_CAPACITY = 10;
constexpr double SERVICE_MIN = 5.0;
constexpr double SERVICE_MAX = 15.0;
constexpr double T_ALPHA = 1.643;
constexpr double DELTA = 0.10;
constexpr int INITIAL_N = 100;
constexpr int MAX_N = 2'000'000;
const array<string, SOURCE_COUNT> SOURCE_NAMES = {"И1 (VIP)", "И2 (Premium)", "И3 (Обычный)"};

struct Request {
    int id = 0;
    int source = 0;
    double arrival = 0.0;
    double serviceStart = 0.0;
    double serviceDuration = 0.0;
};

enum class EventType { ServiceEnd = 0, Arrival = 1 };
struct Event {
    double time = 0.0;
    EventType type = EventType::Arrival;
    int source = -1;
    int device = -1;
    Request request;
    uint64_t seq = 0;
};
struct EventLater {
    bool operator()(const Event& a, const Event& b) const {
        if (a.time != b.time) return a.time > b.time;
        if (a.type != b.type) return static_cast<int>(a.type) > static_cast<int>(b.type);
        return a.seq > b.seq;
    }
};
struct Device {
    optional<Request> request;
    double finishTime = numeric_limits<double>::quiet_NaN();
    double busyTime = 0.0;
};
struct SourceStats {
    int generated = 0;
    int rejected = 0;
    vector<double> waitTimes;
    vector<double> serviceTimes;
    vector<double> systemTimes;
};
struct RunResult {
    array<SourceStats, SOURCE_COUNT> sources;
    array<Device, DEVICE_COUNT> devices;
    double totalTime = 0.0;
    int totalGenerated = 0;
    int totalRejected = 0;
};

class Simulator {
public:
    Simulator(array<double, SOURCE_COUNT> rates, int target, uint32_t seed, bool stepMode)
        : lambda(rates), targetN(target), rng(seed), step(stepMode) {
    }

    RunResult run() {
        RunResult result;
        cout << fixed << setprecision(3);
        if (step) {
            cout << "\nПОШАГОВОЕ МОДЕЛИРОВАНИЕ. Всего заявок: " << targetN << "\n";
            cout << "Нажимайте Enter после каждого особого события.\n";
        }
        for (int s = 0; s < SOURCE_COUNT; ++s) scheduleNextArrival(s, 0.0);
        dispatch(); // Начальный запуск при первоначально свободных приборах.

        int stepNo = 0;
        while (!calendar.empty()) {
            Event e = calendar.top(); calendar.pop();
            if (e.type == EventType::Arrival && generatedTotal >= targetN) continue;
            now = e.time;
            ++stepNo;
            if (step) {
                cout << "\n" << string(78, '=') << "\nШАГ " << stepNo
                     << " | МОДЕЛЬНОЕ ВРЕМЯ: " << now << " мин\n";
            }
            if (e.type == EventType::Arrival) processArrival(e.source);
            else processServiceEnd(e.device, e.request);
            if (step) {
                printState(e);
                if (!calendar.empty()) {
                    cout << "\nEnter — следующее событие...";
                    string line; getline(cin, line);
                }
            }
        }
        result.sources = stats;
        result.totalTime = now;
        result.totalGenerated = generatedTotal;
        result.totalRejected = rejectedTotal;
        result.devices = devices;
        if (step) {
            cout << "\nМОДЕЛИРОВАНИЕ ЗАВЕРШЕНО. Все заявки покинули систему.\n";
            printStepSummary();
        }
        return result;
    }

private:
    array<double, SOURCE_COUNT> lambda;
    int targetN;
    mt19937 rng;
    bool step;
    priority_queue<Event, vector<Event>, EventLater> calendar;
    array<optional<Request>, BUFFER_CAPACITY> buffer{};
    array<Device, DEVICE_COUNT> devices{};
    array<SourceStats, SOURCE_COUNT> stats{};
    int nextGlobalRequestId = 0;
    deque<Request> packagePending;
    int activePackageSource = -1;
    int activeInService = 0;
    int devicePointer = 0; // D2P2: следующий прибор для кольцевого поиска.
    int generatedTotal = 0;
    int rejectedTotal = 0;
    double now = 0.0;
    uint64_t sequence = 0;

    double exponential(double rate) {
        exponential_distribution<double> d(rate);
        return d(rng);
    }
    double uniformService() {
        uniform_real_distribution<double> d(SERVICE_MIN, SERVICE_MAX);
        return d(rng);
    }
    void addEvent(Event e) { e.seq = ++sequence; calendar.push(e); }
    void scheduleNextArrival(int source, double from) {
        if (generatedTotal >= targetN) return;
        Event e; e.time = from + exponential(lambda[source]); e.type = EventType::Arrival; e.source = source;
        addEvent(e);
    }
    int firstFreeSlot() const {
        for (int i = 0; i < BUFFER_CAPACITY; ++i) if (!buffer[i]) return i;
        return -1;
    }
    int oldestSlot() const {
        int idx = -1;
        for (int i = 0; i < BUFFER_CAPACITY; ++i)
            if (buffer[i] && (idx < 0 || buffer[i]->arrival < buffer[idx]->arrival)) idx = i;
        return idx;
    }
    void eraseFromPackage(int id) {
        packagePending.erase(remove_if(packagePending.begin(), packagePending.end(),
            [id](const Request& r) { return r.id == id; }), packagePending.end());
    }
    void removeFromBuffer(int id) {
        for (auto& slot : buffer) if (slot && slot->id == id) { slot.reset(); return; }
    }
    int highestPrioritySource() const {
        for (int s = 0; s < SOURCE_COUNT; ++s)
            for (const auto& slot : buffer) if (slot && slot->source == s) return s;
        return -1;
    }
    int nextFreeDevice() const {
        for (int k = 0; k < DEVICE_COUNT; ++k) {
            int i = (devicePointer + k) % DEVICE_COUNT;
            if (!devices[i].request) return i;
        }
        return -1;
    }
    void discardPendingArrivals() {
        vector<Event> keep;
        while (!calendar.empty()) {
            Event e = calendar.top(); calendar.pop();
            if (e.type == EventType::ServiceEnd) keep.push_back(e);
        }
        for (const Event& e : keep) calendar.push(e);
    }
    void processArrival(int source) {
        Request r;
        r.id = ++nextGlobalRequestId; r.source = source; r.arrival = now;
        ++stats[source].generated; ++generatedTotal;
        if (step) cout << "СОБЫТИЕ: " << SOURCE_NAMES[source] << " сгенерировал заявку " << r.id << ".\n";
        int slot = firstFreeSlot();
        if (slot >= 0) {
            buffer[slot] = r;
            if (step) cout << "Заявка помещена в буфер, место " << slot + 1 << " (первое свободное место).\n";
        } else {
            int old = oldestSlot();
            Request evicted = *buffer[old];
            ++stats[evicted.source].rejected; ++rejectedTotal;
            eraseFromPackage(evicted.id);
            buffer[old] = r;
            if (step) cout << "БУФЕР ПОЛОН: заявка " << evicted.id << " источника "
                           << SOURCE_NAMES[evicted.source] << " вытеснена; новая заявка заняла место " << old + 1 << ".\n";
        }
        scheduleNextArrival(source, now);
        if (generatedTotal >= targetN) discardPendingArrivals();
        dispatch();
    }
    void processServiceEnd(int deviceId, const Request& r) {
        Device& d = devices[deviceId];
        if (!d.request || d.request->id != r.id) return;
        d.request.reset(); d.finishTime = numeric_limits<double>::quiet_NaN();
        --activeInService;
        double wait = r.serviceStart - r.arrival;
        stats[r.source].waitTimes.push_back(wait);
        stats[r.source].serviceTimes.push_back(r.serviceDuration);
        stats[r.source].systemTimes.push_back(now - r.arrival);
        if (step) cout << "СОБЫТИЕ: П" << deviceId + 1 << " завершил заявку " << r.id
                       << " источника " << SOURCE_NAMES[r.source] << "; заявка покинула систему.\n";
        dispatch();
    }
    void formPackage(int source) {
        activePackageSource = source;
        vector<Request> items;
        for (const auto& slot : buffer) if (slot && slot->source == source) items.push_back(*slot);
        sort(items.begin(), items.end(), [](const Request& a, const Request& b) {
            if (a.arrival != b.arrival) return a.arrival < b.arrival;
            return a.id < b.id;
        });
        packagePending.assign(items.begin(), items.end());
        if (step) cout << "ФОРМИРОВАНИЕ ПАКЕТА D2B5: " << SOURCE_NAMES[source]
                       << ", заявок: " << items.size() << ". Состав пакета зафиксирован.\n";
    }
    void startService(int deviceId, Request r) {
        double duration = uniformService();
        r.serviceStart = now; r.serviceDuration = duration;
        Device& d = devices[deviceId]; d.request = r; d.finishTime = now + duration; d.busyTime += duration;
        ++activeInService;
        Event e; e.time = d.finishTime; e.type = EventType::ServiceEnd; e.device = deviceId; e.request = r;
        addEvent(e);
        devicePointer = (deviceId + 1) % DEVICE_COUNT;
        if (step) cout << "НАЗНАЧЕНИЕ: заявка " << r.id << " -> П" << deviceId + 1
                       << "; обслуживание " << duration << " мин, завершение в " << d.finishTime << ".\n";
    }
    void dispatch() {
        while (true) {
            if (activePackageSource == -1) {
                int s = highestPrioritySource();
                if (s < 0) return;
                formPackage(s);
            }
            while (!packagePending.empty()) {
                int dev = nextFreeDevice();
                if (dev < 0) break;
                Request r = packagePending.front(); packagePending.pop_front();
                removeFromBuffer(r.id);
                startService(dev, r);
            }
            if (!packagePending.empty() || activeInService > 0) return;
            activePackageSource = -1; // Пакет полностью завершён.
        }
    }
    void printState(const Event&) const {
        cout << "\nПРИБОРЫ\n" << left << setw(8) << "Прибор" << setw(15) << "Состояние"
             << setw(15) << "Заявка" << "Завершение\n";
        for (int i = 0; i < DEVICE_COUNT; ++i) {
            cout << left << setw(8) << ("P" + to_string(i + 1));
            if (devices[i].request)
                cout << setw(15) << "Занят" << setw(15) << devices[i].request->id << devices[i].finishTime << "\n";
            else cout << setw(15) << "Свободен" << setw(15) << "-" << "-\n";
        }
        cout << "\nБУФЕР (10 МЕСТ)\n" << left << setw(8) << "Место" << setw(12) << "Заявка" << setw(18) << "Источник" << "Время поступления\n";
        for (int i = 0; i < BUFFER_CAPACITY; ++i) {
            cout << left << setw(8) << i + 1;
            if (buffer[i]) cout << setw(12) << buffer[i]->id << setw(18) << SOURCE_NAMES[buffer[i]->source] << buffer[i]->arrival << "\n";
            else cout << setw(12) << "-" << setw(18) << "Свободно" << "-\n";
        }
        cout << "\nПАКЕТ И УКАЗАТЕЛЬ\n";
        cout << "Активный пакет: " << (activePackageSource < 0 ? "нет" : SOURCE_NAMES[activePackageSource])
             << "; ожидает в пакете: " << packagePending.size() << "\n";
        cout << "D2P2: следующий прибор P" << devicePointer + 1 << "\n";
        cout << "\nКАЛЕНДАРЬ СОБЫТИЙ\n";
        auto copy = calendar;
        if (copy.empty()) cout << "Событий нет.\n";
        int shown = 0;
        while (!copy.empty() && shown < 12) {
            Event x = copy.top(); copy.pop();
            cout << setw(10) << x.time << "  ";
            if (x.type == EventType::Arrival)
                cout << "Генерация заявки источника " << SOURCE_NAMES[x.source];
            else cout << "Завершение обслуживания на P" << x.device + 1 << " (заявка " << x.request.id << ")";
            cout << "\n"; ++shown;
        }
        if (copy.size() > 0) cout << "... и ещё " << copy.size() << " событий\n";
        cout << "\nСгенерировано: " << generatedTotal << "/" << targetN
             << "; вытеснено: " << rejectedTotal << "\n";
    }
    void printStepSummary() const {
        cout << "\nИТОГОВЫЕ СЧЁТЧИКИ\n";
        for (int s = 0; s < SOURCE_COUNT; ++s)
            cout << SOURCE_NAMES[s] << ": сгенерировано " << stats[s].generated
                 << ", отказов " << stats[s].rejected << "\n";
    }
};

double mean(const vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0; for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}
double variance(const vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = mean(v), sum = 0.0;
    for (double x : v) sum += (x - m) * (x - m);
    return sum / static_cast<double>(v.size());
}

int estimateNextN(double p, int currentN) {
    if (p <= 0.0) return min(MAX_N, max(currentN + 1, currentN * 2));
    if (p >= 1.0) return currentN;
    double raw = (T_ALPHA * T_ALPHA * (1.0 - p)) / (p * DELTA * DELTA);
    long long proposed = static_cast<long long>(ceil(raw));
    proposed = max<long long>(proposed, currentN);
    return static_cast<int>(min<long long>(proposed, MAX_N));
}

void printFinalTables(const RunResult& r) {
    cout << "\n\nТАБЛИЦА 1. ХАРАКТЕРИСТИКИ ИСТОЧНИКОВ ВС\n";
    cout << left << setw(20) << "Источник" << right << setw(10) << "N" << " "
         << setw(13) << "p отказа" << setw(14) << "Tпреб"
         << setw(14) << "TБП" << setw(14) << "Tобсл"
         << setw(14) << "ДБП" << setw(14) << "Добсл" << "\n";
    for (int s = 0; s < SOURCE_COUNT; ++s) {
        const auto& x = r.sources[s];
        double p = x.generated ? static_cast<double>(x.rejected) / x.generated : 0.0;
        cout << left << setw(20) << SOURCE_NAMES[s] << right << setw(10) << x.generated << " "
             << setw(13) << p << setw(14) << mean(x.systemTimes)
             << setw(14) << mean(x.waitTimes) << setw(14) << mean(x.serviceTimes)
             << setw(14) << variance(x.waitTimes) << setw(14) << variance(x.serviceTimes) << "\n";
    }
    cout << "\nТАБЛИЦА 2. КОЭФФИЦИЕНТЫ ИСПОЛЬЗОВАНИЯ ПРИБОРОВ\n";
    for (int d = 0; d < DEVICE_COUNT; ++d) {
        double u = r.totalTime > 0.0 ? r.devices[d].busyTime / r.totalTime : 0.0;
        cout << "P" << d + 1 << ": " << u << "\n";
    }
    cout << "Общее время реализации: " << r.totalTime << " мин\n";
    cout << "Всего заявок: " << r.totalGenerated << "; вытеснено: " << r.totalRejected << "\n";
}

} // namespace

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cout << "МОДЕЛЬ ЭЛЕКТРОННОЙ ОЧЕРЕДИ БАНКА (C++)\n";
    cout << "Параметры: 3 источника, 3 прибора, буфер 10, обслуживание U[5;15].\n";
    cout << "Точность delta=10%, доверительная вероятность alpha=0.9, t_alpha=1.643.\n\n";

    array<double, SOURCE_COUNT> rates{};
    for (int s = 0; s < SOURCE_COUNT; ++s) {
        while (true) {
            cout << "Введите интенсивность lambda для " << SOURCE_NAMES[s] << " (заявок/мин): ";
            if (cin >> rates[s] && isfinite(rates[s]) && rates[s] > 0.0) break;
            cout << "Ошибка: требуется положительное число.\n";
            cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n');
        }
    }
    cout << "\nВыберите режим: 1 — пошаговый; 2 — автоматический с подбором N: ";
    int mode; if (!(cin >> mode) || (mode != 1 && mode != 2)) { cerr << "Неверный режим.\n"; return 1; }

    if (mode == 1) {
        int n;
        cout << "Введите общее число заявок для пошаговой реализации: ";
        if (!(cin >> n) || n <= 0) { cerr << "N должно быть положительным.\n"; return 1; }
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        Simulator sim(rates, n, random_device{}(), true);
        RunResult result = sim.run();
        printFinalTables(result);
        return 0;
    }

    int n = INITIAL_N;
    RunResult previousResult;
    bool havePrevious = false;
    RunResult finalResult;
    cout << "\nАВТОМАТИЧЕСКИЙ ПОДБОР ЧИСЛА ЗАЯВОК\n";
    cout << "Начальное N0=" << INITIAL_N << "; сравнение относительного изменения p с 10%.\n";
    bool converged = false;
    for (int iteration = 0; iteration < 30; ++iteration) {
        Simulator sim(rates, n, random_device{}(), false);
        RunResult current = sim.run();
        double p = current.totalGenerated ? static_cast<double>(current.totalRejected) / current.totalGenerated : 0.0;
        cout << "Итерация " << iteration + 1 << ": N=" << n << ", p=" << setprecision(8) << p;
        if (havePrevious) {
            double relativeChange = previousResult.totalRejected > 0
                ? abs(p - static_cast<double>(previousResult.totalRejected) / previousResult.totalGenerated)
                    / (static_cast<double>(previousResult.totalRejected) / previousResult.totalGenerated)
                : numeric_limits<double>::infinity();
            cout << ", относительное изменение=" << relativeChange;
            if (relativeChange < DELTA) {
                cout << " — точность достигнута. Принимается предыдущая реализация N="
                     << previousResult.totalGenerated << ".\n";
                finalResult = move(previousResult);
                converged = true;
                break;
            }
        }
        cout << "\n";
        if (n >= MAX_N) {
            cerr << "Достигнут предельный объём моделирования, критерий точности не подтверждён.\n";
            finalResult = move(current);
            break;
        }
        previousResult = move(current);
        havePrevious = true;
        double currentP = previousResult.totalGenerated
            ? static_cast<double>(previousResult.totalRejected) / previousResult.totalGenerated : 0.0;
        int nextN = estimateNextN(currentP, n);
        cout << "Следующее количество заявок N=" << nextN << " по формуле (2).\n";
        n = nextN;
        finalResult = previousResult;
        if (iteration == 29) cerr << "Достигнут лимит итераций; точность не подтверждена.\n";
    }
    if (!converged && finalResult.totalGenerated == 0) {
        cerr << "Не удалось получить результат моделирования.\n";
        return 2;
    }
    printFinalTables(finalResult);
    return 0;
}
