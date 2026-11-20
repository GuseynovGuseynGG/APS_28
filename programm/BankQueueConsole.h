#pragma once

#include "BankQueueSimulation.h"

#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bank_queue {

enum class Mode { Step, Automatic };

struct Options {
    SimulationConfig config;
    Mode mode;
    std::optional<int> requests;
    bool pause = true;
};

constexpr int INITIAL_N = 100;
constexpr int MAX_N = 2'000'000;
constexpr double DELTA = 0.10;
constexpr double T_ALPHA = 1.643;

inline int estimateNextN(double probability, int currentN) {
    if (probability == 0.0) return std::min(MAX_N, 2 * currentN);
    const double estimate = std::ceil(T_ALPHA * T_ALPHA * (1.0 - probability)
                                      / (probability * DELTA * DELTA));
    return std::max(currentN, static_cast<int>(std::min(estimate, static_cast<double>(MAX_N))));
}

inline void printFinalTables(const RunResult& result) {
    std::cout << "\nОР1. ХАРАКТЕРИСТИКИ ИСТОЧНИКОВ\n"
              << column("Источник", 15) << column("Пришло", 8) << column("Обслуж.", 8)
              << column("Отказов", 8) << column("P отк.", 8) << column("T ож.", 9)
              << column("T обсл.", 9) << column("T преб.", 9) << column("D ож.", 9)
              << column("D обсл.", 9) << "D преб.\n";
    int totalServed = 0;
    double totalSystemTime = 0.0;
    const auto meanText = [](const Moments& values) {
        return values.count == 0 ? "—" : number(values.mean);
    };
    const auto varianceText = [](const Moments& values) {
        return values.count == 0 ? "—" : number(values.variance());
    };
    for (int i = 0; i < SOURCE_COUNT; ++i) {
        const auto& stats = result.sources[i];
        totalServed += stats.served;
        totalSystemTime += stats.system.mean * stats.system.count;
        std::cout << column(SOURCE_NAMES[i], 15) << column(std::to_string(stats.generated), 8)
                  << column(std::to_string(stats.served), 8) << column(std::to_string(stats.rejected), 8)
                  << column(stats.generated == 0 ? "—" : number(stats.rejectionProbability(), 5), 8)
                  << column(meanText(stats.wait), 9) << column(meanText(stats.service), 9)
                  << column(meanText(stats.system), 9) << column(varianceText(stats.wait), 9)
                  << column(varianceText(stats.service), 9) << varianceText(stats.system) << '\n';
    }
    std::cout << "T — среднее в минутах, D — дисперсия в минутах², P отк. — доля отказов.\n";

    std::cout << "\nОР1. ИСПОЛЬЗОВАНИЕ ОКОН\n"
              << column("Окно", 9) << column("Занято, мин", 15) << "Коэффициент\n";
    double totalBusyTime = 0.0;
    for (std::size_t i = 0; i < result.devices.size(); ++i) {
        const double busyTime = result.devices[i].busyTime;
        totalBusyTime += busyTime;
        std::cout << column(deviceCode(static_cast<int>(i)), 9) << column(number(busyTime), 15)
                  << number(busyTime / result.totalTime, 5) << '\n';
    }
    const double meanUtilization = totalBusyTime / result.totalTime / result.devices.size();
    std::cout << "\nВсего заявок: " << result.totalGenerated << "; обслужено: " << totalServed
              << "; отказов: " << result.totalRejected << "; P отк.: " << number(result.rejectionProbability(), 5)
              << "\nОбщее время реализации: " << number(result.totalTime) << " мин"
              << "\nСреднее время пребывания: " << number(totalSystemTime / result.totalGenerated) << " мин"
              << "\nСредняя загрузка окон: " << number(100.0 * meanUtilization, 3) << "%\n";
}

inline RunResult runAutomatic(SimulationConfig config) {
    std::cout << "\nАВТОМАТИЧЕСКИЙ ПОДБОР ЧИСЛА ЗАЯВОК\n"
              << "N0 = " << INITIAL_N << ", δ = 10%, α = 0.9, tα = 1.643.\n";
    std::mt19937 iterationSeeds(config.seed);
    std::optional<RunResult> previous;
    int n = INITIAL_N;
    while (true) {
        config.seed = iterationSeeds();
        RunResult current = Simulator(config, n).run();
        const double p = current.rejectionProbability();
        std::cout << "N = " << n << ", seed = " << config.seed << ", p = " << number(p, 8);

        if (previous && previous->totalRejected > 0 && p > 0.0) {
            const double previousP = previous->rejectionProbability();
            const double relativeChange = std::abs(p - previousP) / previousP;
            std::cout << ", относительное изменение = " << number(100.0 * relativeChange, 3) << '%';
            if (relativeChange < DELTA) {
                std::cout << "\nКритерий точности выполнен. Принята предыдущая реализация N = "
                          << previous->totalGenerated << ".\n";
                return std::move(*previous);
            }
        }
        std::cout << '\n';

        if (n == MAX_N) {
            std::cout << "Достигнут предел N = " << MAX_N << ". Критерий точности не подтверждён.\n";
            return current;
        }
        int nextN = estimateNextN(p, n);
        if (previous && nextN == n) nextN = std::min(MAX_N, 2 * n);
        std::cout << "Следующий объём N = " << nextN;
        if (p == 0.0) std::cout << " (отказов пока нет, объём удвоен)";
        std::cout << ".\n" << std::flush;
        previous = std::move(current);
        n = nextN;
    }
}

template <typename Integer>
Integer readInteger(std::string_view text, const std::string& name) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument(name + ": требуется целое число в допустимом диапазоне.");
    }
    return value;
}

inline std::string readToken(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string token;
    if (!(std::cin >> token)) throw std::runtime_error("Ввод завершён.");
    return token;
}

inline int readPositiveInteger(std::string_view text, const std::string& name) {
    const int value = readInteger<int>(text, name);
    if (value <= 0) throw std::invalid_argument(name + ": требуется положительное целое число.");
    return value;
}

inline double readPositiveDouble(const std::string& text, const std::string& name) {
    const auto error = std::invalid_argument(name + ": требуется положительное конечное число.");
    std::size_t consumed = 0;
    double value;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::invalid_argument&) {
        throw error;
    } catch (const std::out_of_range&) {
        throw error;
    }
    if (consumed != text.size() || !std::isfinite(value) || value <= 0.0) throw error;
    return value;
}

inline void printUsage(Mode mode) {
    const char* program = mode == Mode::Step ? "BankQueueStepMode" : "BankQueueAutoMode";
    std::cout << program << " --rates L1 L2 L3"
              << (mode == Mode::Step ? " --requests N [--no-pause]" : " [--requests N]")
              << " [--windows K] [--buffer B] [--seed S]\n"
              << "    [--service MIN MAX] [--operation-service ID MIN MAX]\n"
              << "L — интенсивности в заявках/мин, MIN и MAX — границы обслуживания в минутах.\n"
              << "ID операции: 1 — вклад, 2 — кредит, 3 — консультация, 4 — обслуживание юрлиц.\n"
              << "При запуске без аргументов параметры запрашиваются в консоли.\n";
    if (mode == Mode::Automatic) std::cout << "Без --requests программа автоматически подбирает N.\n";
}

inline Options readInteractive(Mode mode) {
    Options options{SimulationConfig{}, mode, std::nullopt, true};
    for (int i = 0; i < SOURCE_COUNT; ++i) {
        const std::string prompt = "Введите интенсивность для " + SOURCE_NAMES[i] + " (заявок/мин): ";
        while (true) {
            const auto token = readToken(prompt);
            try {
                options.config.rates[i] = readPositiveDouble(token, "Интенсивность");
                break;
            } catch (const std::invalid_argument& error) {
                std::cout << error.what() << '\n';
            }
        }
    }
    if (mode == Mode::Step) {
        const int selection = readInteger<int>(
            readToken("Режим (1 — пошаговый, 2 — автоматический): "), "Режим");
        if (selection != 1 && selection != 2) throw std::invalid_argument("Режим должен быть 1 или 2.");
        options.mode = selection == 1 ? Mode::Step : Mode::Automatic;
    }
    if (options.mode == Mode::Step) {
        options.requests = readPositiveInteger(readToken("Общее число заявок: "), "N");
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    options.config.seed = std::random_device{}();
    return options;
}

inline Options readOptions(int argc, char** argv, Mode mode) {
    if (argc == 1) return readInteractive(mode);

    Options options{SimulationConfig{}, mode, std::nullopt, true};
    bool ratesProvided = false;
    std::optional<std::uint32_t> seed;
    ServiceInterval defaultInterval;
    std::array<std::optional<ServiceInterval>, OPERATION_COUNT> operationIntervals{};
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const auto next = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("Недостаточно аргументов для " + option + ".");
            return argv[i];
        };
        const auto interval = [&]() {
            const double minimum = readPositiveDouble(next(), option);
            const double maximum = readPositiveDouble(next(), option);
            if (maximum <= minimum) throw std::invalid_argument(option + ": требуется MIN < MAX.");
            return ServiceInterval{minimum, maximum};
        };

        if (option == "--rates") {
            for (double& rate : options.config.rates) rate = readPositiveDouble(next(), option);
            ratesProvided = true;
        } else if (option == "--requests") {
            options.requests = readPositiveInteger(next(), option);
        } else if (option == "--windows") {
            options.config.windows = readPositiveInteger(next(), option);
        } else if (option == "--buffer") {
            options.config.bufferCapacity = readPositiveInteger(next(), option);
        } else if (option == "--seed") {
            seed = readInteger<std::uint32_t>(next(), option);
        } else if (option == "--service") {
            defaultInterval = interval();
        } else if (option == "--operation-service") {
            const int operation = readPositiveInteger(next(), option);
            if (operation > OPERATION_COUNT) throw std::invalid_argument(option + ": требуется ID от 1 до 4.");
            operationIntervals[operation - 1] = interval();
        } else if (option == "--no-pause" && mode == Mode::Step) {
            options.pause = false;
        } else {
            throw std::invalid_argument("Неизвестный аргумент " + option + ". Используйте --help.");
        }
    }
    if (!ratesProvided) throw std::invalid_argument("Укажите --rates L1 L2 L3.");
    if (mode == Mode::Step && !options.requests) throw std::invalid_argument("Укажите --requests N.");

    for (int i = 0; i < OPERATION_COUNT; ++i) {
        options.config.service[i] = operationIntervals[i].value_or(defaultInterval);
    }
    options.config.seed = seed ? *seed : std::random_device{}();
    return options;
}

inline void printConfig(const SimulationConfig& config) {
    std::cout << "Окон: " << config.windows << "; мест в буфере: " << config.bufferCapacity
              << "; seed: " << config.seed << "\nИнтенсивности, заявок/мин:";
    for (double rate : config.rates) std::cout << ' ' << rate;
    std::cout << "\nОбслуживание по операциям, мин:";
    for (int i = 0; i < OPERATION_COUNT; ++i) {
        const auto interval = config.service[i];
        std::cout << ' ' << i + 1 << "=[" << interval.minimum << ", " << interval.maximum << ']';
    }
    std::cout << '\n' << std::flush;
}

inline int runConsole(int argc, char** argv, Mode entryMode) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        printUsage(entryMode);
        return 0;
    }
    try {
        std::cout << "ЭЛЕКТРОННАЯ ОЧЕРЕДЬ БАНКА · ВАРИАНТ 28\n";
        const Options options = readOptions(argc, argv, entryMode);
        printConfig(options.config);
        if (options.mode == Mode::Step) {
            std::cout << "ОД1. ПОШАГОВЫЙ РЕЖИМ. Заявок: " << *options.requests << "\n";
            Simulator simulator(options.config, *options.requests, &std::cout,
                                options.pause ? &std::cin : nullptr);
            const RunResult result = simulator.run();
            std::cout << "\nМоделирование завершено. Все заявки покинули систему.\n";
            printFinalTables(result);
        } else if (options.requests) {
            std::cout << "ОР1. ОДНА РЕАЛИЗАЦИЯ. Заявок: " << *options.requests << "\n";
            printFinalTables(Simulator(options.config, *options.requests).run());
        } else {
            printFinalTables(runAutomatic(options.config));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 1;
    }
}

}
