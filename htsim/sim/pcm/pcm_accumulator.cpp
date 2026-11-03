// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_accumulator.h"

#include <algorithm>
#include <cmath>

PcmAccumulator::PcmAccumulator(PcmAccOp op, double smoothing)
    : _op(op), _s(smoothing), _value(op == PcmAccOp::MIN ? kPcmMinStartValue : 0) {}

void PcmAccumulator::update(int64_t x) {
    switch (_op) {
    case PcmAccOp::SUM:
        _value += x;
        break;
    case PcmAccOp::MAX:
        _value = std::max(_value, x);
        break;
    case PcmAccOp::MIN:
        _value = std::min(_value, x);
        break;
    case PcmAccOp::LAST:
        _value = x;
        break;
    case PcmAccOp::EWMA:
        // Table 3-107: ewma = x * s + ewma * (1 - s)
        _fvalue = _ewma_started ? double(x) * _s + _fvalue * (1 - _s) : double(x);
        _ewma_started = true;
        break;
    }
}

void PcmAccumulator::set(int64_t x) {
    if (_op == PcmAccOp::EWMA) {
        _fvalue = double(x);
        _ewma_started = true;
    } else {
        _value = x;
    }
}

int64_t PcmAccumulator::get() const {
    if (_op == PcmAccOp::EWMA)
        return int64_t(std::llround(_fvalue));
    return _value;
}

double PcmAccumulator::getFloat() const {
    if (_op == PcmAccOp::EWMA)
        return _fvalue;
    return double(_value);
}
