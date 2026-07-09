#include "atmsim/engine/ServiceStages.hpp"

namespace atmsim {

// ---------------------------------------------------------------------------
//  Планы этапов. Доли подобраны «по ощущению правдоподобия»: PIN и меню —
//  короткие, отсчёт/пересчёт купюр — самые длинные механические операции,
//  чек и возврат карты — быстрый финал. Инвариант каждого плана: сумма
//  fraction == 1.0 (закреплено тестом test_service_stages).
//
//  ВАЖНО: первый этап всегда InsertCard, последний — всегда ReturnCard: клиент
//  не может уйти без карты, а обслуживание не может начаться без неё.
// ---------------------------------------------------------------------------
const std::vector<StageSlice>& serviceStagePlan(OperationType op) {
    // Запрос баланса: никакой возни с наличными — почти всё время делят
    // экран с балансом и общение с банком.
    static const std::vector<StageSlice> checkBalance{
        {ServiceStage::InsertCard,      0.10},
        {ServiceStage::EnterPin,        0.18},
        {ServiceStage::SelectOperation, 0.12},
        {ServiceStage::BankRequest,     0.15},
        {ServiceStage::ShowBalance,     0.20},
        {ServiceStage::PrintReceipt,    0.13},
        {ServiceStage::ReturnCard,      0.12},
    };
    // Снятие: после подтверждения банка банкомат отсчитывает купюры (самый
    // долгий этап), клиент забирает деньги, затем чек и карта.
    static const std::vector<StageSlice> withdraw{
        {ServiceStage::InsertCard,      0.08},
        {ServiceStage::EnterPin,        0.14},
        {ServiceStage::SelectOperation, 0.10},
        {ServiceStage::EnterAmount,     0.12},
        {ServiceStage::BankRequest,     0.10},
        {ServiceStage::CountCash,       0.18},
        {ServiceStage::DispenseCash,    0.13},
        {ServiceStage::PrintReceipt,    0.07},
        {ServiceStage::ReturnCard,      0.08},
    };
    // Внесение: клиент вкладывает купюры, банкомат их пересчитывает и лишь
    // потом подтверждает зачисление в банке.
    static const std::vector<StageSlice> deposit{
        {ServiceStage::InsertCard,      0.08},
        {ServiceStage::EnterPin,        0.14},
        {ServiceStage::SelectOperation, 0.10},
        {ServiceStage::EnterAmount,     0.10},
        {ServiceStage::InsertCash,      0.20},
        {ServiceStage::VerifyCash,      0.18},
        {ServiceStage::BankRequest,     0.07},
        {ServiceStage::PrintReceipt,    0.06},
        {ServiceStage::ReturnCard,      0.07},
    };

    switch (op) {
        case OperationType::Withdraw: return withdraw;
        case OperationType::Deposit:  return deposit;
        case OperationType::CheckBalance: break;
    }
    return checkBalance;
}

ServiceStage serviceStageAt(OperationType op, double progress) {
    const std::vector<StageSlice>& plan = serviceStagePlan(op);
    // Идём по плану, накапливая границы этапов; первый этап, чья верхняя
    // граница превысила progress, и есть текущий. Значения progress <= 0 дают
    // первый этап, >= 1 (включая джиттер сверх суммы долей) — последний.
    double upper = 0.0;
    for (const StageSlice& s : plan) {
        upper += s.fraction;
        if (progress < upper) return s.stage;
    }
    return plan.back().stage;
}

std::string to_string(ServiceStage s) {
    // Формулировки от третьего лица — подставляются после «Клиент #N: ...»;
    // шаги самого банкомата — безличные («идёт отсчёт купюр»). Названия
    // НАМЕРЕННО короткие, не длиннее 19 колонок: строка этапа на дашборде
    // « └ имя [██████████] 100%» обязана влезать в левую колонку даже при её
    // минимальной ширине 40 (терминал 80 колонок), иначе fit() отрежет бар и
    // процент. Закреплено тестом stage_names_are_human_readable.
    switch (s) {
        case ServiceStage::InsertCard:      return "вставляет карту";
        case ServiceStage::EnterPin:        return "вводит PIN-код";
        case ServiceStage::SelectOperation: return "выбирает операцию";
        case ServiceStage::EnterAmount:     return "вводит сумму";
        case ServiceStage::BankRequest:     return "ждёт ответа банка";
        case ServiceStage::CountCash:       return "идёт отсчёт купюр";
        case ServiceStage::DispenseCash:    return "забирает наличные";
        case ServiceStage::InsertCash:      return "вкладывает купюры";
        case ServiceStage::VerifyCash:      return "идёт пересчёт купюр";
        case ServiceStage::ShowBalance:     return "смотрит баланс";
        case ServiceStage::PrintReceipt:    return "печатается чек";
        case ServiceStage::ReturnCard:      return "забирает карту";
    }
    return "?";
}

}  // namespace atmsim
