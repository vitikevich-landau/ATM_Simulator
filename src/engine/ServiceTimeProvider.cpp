#include "atmsim/engine/ServiceTimeProvider.hpp"

namespace atmsim {
namespace {

// Равномерное распределение на [min, max].
class UniformServiceTime : public ServiceTimeProvider {
public:
    UniformServiceTime(double minSec, double maxSec) : min_(minSec), max_(maxSec) {}
    double nextSeconds(OperationType, std::mt19937_64& rng) override {
        std::uniform_real_distribution<double> dist(min_, max_);
        return dist(rng);
    }
private:
    double min_, max_;
};

// Нормальное (гауссово) распределение со средним mean и разбросом stddev.
// Усекаем снизу величиной min: время обслуживания не может быть меньше нуля
// (и меньше разумного минимума), а «хвост» нормального распределения туда лезет.
class NormalServiceTime : public ServiceTimeProvider {
public:
    NormalServiceTime(double mean, double stddev, double minSec)
        : mean_(mean), stddev_(stddev), min_(minSec) {}
    double nextSeconds(OperationType, std::mt19937_64& rng) override {
        std::normal_distribution<double> dist(mean_, stddev_);
        const double v = dist(rng);
        return v < min_ ? min_ : v;
    }
private:
    double mean_, stddev_, min_;
};

// Экспоненциальное распределение со средним mean (параметр lambda = 1/mean).
// Тоже усекаем снизу величиной min.
class ExponentialServiceTime : public ServiceTimeProvider {
public:
    ExponentialServiceTime(double mean, double minSec)
        : lambda_(mean > 0.0 ? 1.0 / mean : 1.0), min_(minSec) {}
    double nextSeconds(OperationType, std::mt19937_64& rng) override {
        std::exponential_distribution<double> dist(lambda_);
        const double v = dist(rng);
        return v < min_ ? min_ : v;
    }
private:
    double lambda_, min_;
};

}  // namespace

std::unique_ptr<ServiceTimeProvider> makeServiceTimeProvider(const ServiceTimeConfig& cfg) {
    switch (cfg.distribution) {
        case ServiceDistribution::Uniform:
            return std::make_unique<UniformServiceTime>(cfg.minSeconds, cfg.maxSeconds);
        case ServiceDistribution::Normal:
            return std::make_unique<NormalServiceTime>(cfg.meanSeconds, cfg.stddevSeconds, cfg.minSeconds);
        case ServiceDistribution::Exponential:
            return std::make_unique<ExponentialServiceTime>(cfg.meanSeconds, cfg.minSeconds);
    }
    // На случай, если enum пополнится, а ветку забудут добавить.
    return std::make_unique<NormalServiceTime>(cfg.meanSeconds, cfg.stddevSeconds, cfg.minSeconds);
}

}  // namespace atmsim
