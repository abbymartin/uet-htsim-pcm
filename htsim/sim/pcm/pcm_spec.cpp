// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pcm_spec.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace {

// ---------------------------------------------------------------------------
// Helpers

class Parser {
public:
    explicit Parser(const std::string& source) : _source(source) {}

    [[noreturn]] void fail(const YAML::Node& at, const std::string& msg) const {
        std::ostringstream os;
        os << _source;
        if (at.IsDefined() && at.Mark().line >= 0)
            os << ":" << at.Mark().line + 1;
        os << ": " << msg;
        throw PcmSpecError(os.str());
    }

    [[noreturn]] void failTop(const std::string& msg) const {
        throw PcmSpecError(_source + ": " + msg);
    }

    static int lineOf(const YAML::Node& n) {
        return (n.IsDefined() && n.Mark().line >= 0) ? n.Mark().line + 1 : 0;
    }

    // Checks that `node` is a mapping whose keys are all in `allowed`, with no
    // key repeated. yaml-cpp accepts repeated keys silently, which would hide
    // mistakes.
    void checkMap(const YAML::Node& node, const std::string& what,
                  const std::set<std::string>& allowed) const {
        if (!node.IsMap())
            fail(node, what + " must be a mapping of key: value pairs");
        std::set<std::string> seen;
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (!seen.insert(key).second)
                fail(it->first, "key '" + key + "' appears twice in " + what);
            if (!allowed.count(key)) {
                std::string list;
                for (const auto& a : allowed)
                    list += (list.empty() ? "" : ", ") + a;
                fail(it->first, "unknown key '" + key + "' in " + what +
                                    " (allowed keys: " + list + ")");
            }
        }
    }

    YAML::Node required(const YAML::Node& map, const std::string& key,
                        const std::string& what) const {
        YAML::Node n = map[key];
        if (!n.IsDefined() || n.IsNull())
            fail(map, what + " is missing required key '" + key + "'");
        return n;
    }

    std::string scalar(const YAML::Node& n, const std::string& what) const {
        if (!n.IsScalar())
            fail(n, what + " must be a single value");
        return n.Scalar();
    }

    // Unsigned integer: decimal, or hex with a 0x prefix (the spec writes ranges
    // like 0xFFFF). A leading 0 is not treated as octal.
    uint64_t toUint(const YAML::Node& n, const std::string& what) const {
        return parseUint(n, scalar(n, what), what);
    }

    // `kind` is used in error messages, so that a bad signed value isn't
    // reported as a bad unsigned one.
    uint64_t parseUint(const YAML::Node& at, const std::string& s, const std::string& what,
                       const std::string& kind = "an unsigned integer") const {
        if (s.empty() || s[0] == '-' || s[0] == '+')
            fail(at, what + " must be " + kind + ", got '" + s + "'");
        int base = 10;
        const char* start = s.c_str();
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            start += 2;
        }
        errno = 0;
        char* end = nullptr;
        unsigned long long v = std::strtoull(start, &end, base);
        if (end == start || *end != '\0')
            fail(at, what + " must be " + kind + ", got '" + s + "'");
        if (errno == ERANGE)
            fail(at, what + " is too large for 64 bits: '" + s + "'");
        return v;
    }

    int64_t toInt(const YAML::Node& n, const std::string& what) const {
        const std::string s = scalar(n, what);
        bool neg = !s.empty() && s[0] == '-';
        std::string mag = (neg || (!s.empty() && s[0] == '+')) ? s.substr(1) : s;
        if (mag.empty() || mag[0] == '-' || mag[0] == '+')
            fail(n, what + " must be an integer, got '" + s + "'");
        uint64_t u = parseUint(n, mag, what, "an integer");
        if (neg) {
            if (u > uint64_t(INT64_MAX) + 1)
                fail(n, what + " is too small for 64 bits: '" + s + "'");
            return u == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -int64_t(u);
        }
        if (u > uint64_t(INT64_MAX))
            fail(n, what + " is too large for a signed 64-bit integer: '" + s + "'");
        return int64_t(u);
    }

    double toFloat(const YAML::Node& n, const std::string& what) const {
        const std::string s = scalar(n, what);
        errno = 0;
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (s.empty() || end == s.c_str() || *end != '\0')
            fail(n, what + " must be a number, got '" + s + "'");
        if (errno == ERANGE)
            fail(n, what + " is out of range: '" + s + "'");
        return v;
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // Enum values are matched case-insensitively: the spec's YAML format writes
    // e.g. "CONTINUOUS" and "INVOKE", while its examples write "continuous" and
    // "invoke".
    template <typename E>
    E toEnum(const YAML::Node& n, const std::string& what,
             const std::map<std::string, E>& names) const {
        const std::string s = lower(scalar(n, what));
        auto it = names.find(s);
        if (it == names.end()) {
            std::string list;
            for (const auto& kv : names)
                list += (list.empty() ? "" : ", ") + kv.first;
            fail(n, "invalid " + what + " '" + n.Scalar() + "' (allowed: " + list + ")");
        }
        return it->second;
    }

    std::string identifier(const YAML::Node& n, const std::string& what) const {
        const std::string s = scalar(n, what);
        bool ok = !s.empty() && (std::isalpha((unsigned char)s[0]) || s[0] == '_');
        for (char c : s)
            ok = ok && (std::isalnum((unsigned char)c) || c == '_');
        if (!ok)
            fail(n, what + " must be a C identifier, got '" + s + "'");
        return s;
    }

    // Each section in the spec's YAML format is described as a "sequence node",
    // but the spec's own examples (§3.6.18.5.5, §3.6.18.8.5) write a single
    // signal as a plain mapping without a list. Both forms are accepted.
    // See docs/pcm-plan.md §7.
    std::vector<YAML::Node> entries(const YAML::Node& n, const std::string& what) const {
        std::vector<YAML::Node> out;
        if (n.IsSequence()) {
            for (const auto& e : n)
                out.push_back(e);
        } else if (n.IsMap()) {
            out.push_back(n);
        } else {
            fail(n, what + " must be a list of entries");
        }
        return out;
    }

    const std::string& source() const { return _source; }

private:
    std::string _source;
};

const std::map<std::string, PcmDtype> kDtypes = {
    {"uint", PcmDtype::UINT}, {"int", PcmDtype::INT}, {"float", PcmDtype::FLOAT}};

PcmValue parseValue(const Parser& p, const YAML::Node& n, PcmDtype dtype,
                    const std::string& what) {
    switch (dtype) {
    case PcmDtype::UINT:
        return p.toUint(n, what);
    case PcmDtype::INT:
        return p.toInt(n, what);
    case PcmDtype::FLOAT:
        return p.toFloat(n, what);
    }
    return uint64_t(0);
}

// ---------------------------------------------------------------------------
// Sections

PcmAlgorithmSpec parseAlgorithm(const Parser& p, const YAML::Node& n) {
    const std::string what = "cc_algorithm";
    p.checkMap(n, what,
               {"name", "description", "max_ack_coalescing", "uint_min_bitwidth",
                "int_min_bitwidth", "float_min_bitwidth"});
    PcmAlgorithmSpec a;
    a.name = p.identifier(p.required(n, "name", what), what + ".name");
    a.description = p.scalar(p.required(n, "description", what), what + ".description");
    // max_ack_coalescing is read and checked but not yet used by the simulator;
    // see docs/pcm-plan.md §7.
    a.max_ack_coalescing =
        p.toUint(p.required(n, "max_ack_coalescing", what), what + ".max_ack_coalescing");

    // The simulator stores integers in 64 bits and floats as double, so any
    // minimum bit width up to 64 is met.
    auto bitwidth = [&](const char* key) -> std::optional<uint64_t> {
        YAML::Node b = n[key];
        if (!b.IsDefined())
            return std::nullopt;
        uint64_t v = p.toUint(b, what + "." + key);
        if (v == 0 || v > 64)
            p.fail(b, what + "." + key + " must be between 1 and 64; the simulator "
                      "uses 64-bit integers and 64-bit floats");
        return v;
    };
    a.uint_min_bitwidth = bitwidth("uint_min_bitwidth");
    a.int_min_bitwidth = bitwidth("int_min_bitwidth");
    a.float_min_bitwidth = bitwidth("float_min_bitwidth");
    return a;
}

PcmTriggerSpec parseTrigger(const Parser& p, const YAML::Node& n, const std::string& what,
                            PcmSpec& spec) {
    p.checkMap(n, what,
               {"op", "op_val", "trigger_type", "mode", "mode_refval", "arm_mode", "action",
                "invoke_deadline"});
    PcmTriggerSpec t;

    t.op = p.toEnum<PcmTriggerOp>(p.required(n, "op", what), what + ".op",
                                  {{"eq", PcmTriggerOp::EQ},
                                   {"neq", PcmTriggerOp::NEQ},
                                   {"gt", PcmTriggerOp::GT},
                                   {"gte", PcmTriggerOp::GTE},
                                   {"lt", PcmTriggerOp::LT},
                                   {"lte", PcmTriggerOp::LTE},
                                   {"wc", PcmTriggerOp::WC}});

    // The spec is inconsistent about what a trigger compares against. Its
    // trigger definition (§3.6.18.5.1) compares the accumulator against the
    // mode_refval numbers that also set the count and time thresholds, and its
    // YAML format (§3.6.18.8.5) has no separate value, but its own example
    // (§3.6.18.5.5) uses op_val. Read literally, "fire as soon as RTT exceeds
    // X" can't be expressed. We always compare `accumulator (op) op_val`, and
    // op_val is required for every operator except the wildcard.
    // See docs/pcm-plan.md §7.
    YAML::Node op_val = n["op_val"];
    if (t.op == PcmTriggerOp::WC) {
        if (op_val.IsDefined())
            spec.warnings.push_back(p.source() + ":" + std::to_string(Parser::lineOf(op_val)) +
                                    ": " + what + ".op_val is ignored because op is wc");
    } else {
        t.op_val = p.toInt(p.required(n, "op_val", what), what + ".op_val");
    }

    // trigger_type is not in the spec, which only has the "raw" behaviour
    // (compare the accumulator's value). It comes from the PCM paper (Khalilov
    // et al. §5.1), whose trigger modes are RAW, DELTA (compare the change since
    // the trigger's last satisfied check) and MAGNITUDE (the absolute value of
    // that change). The paper's own handlers use them, e.g. its MP-RDMA YAML
    // (App. B, Listing 4) has "trigger_type: DELTA". See docs/pcm-plan.md §7.
    YAML::Node type = n["trigger_type"];
    if (type.IsDefined()) {
        t.type = p.toEnum<PcmTriggerType>(type, what + ".trigger_type",
                                          {{"raw", PcmTriggerType::RAW},
                                           {"delta", PcmTriggerType::DELTA},
                                           {"magnitude", PcmTriggerType::MAGNITUDE}});
        if (t.type != PcmTriggerType::RAW)
            spec.extensions_used.push_back("trigger_type " + Parser::lower(type.Scalar()) +
                                           " (" + what + ", line " +
                                           std::to_string(Parser::lineOf(type)) + ")");
    }

    // count_or_time is the only mode the spec defines (§3.6.18.5.1).
    p.toEnum<int>(p.required(n, "mode", what), what + ".mode", {{"count_or_time", 0}});

    // mode_refval: "<count> <time>", two unsigned integers separated by
    // whitespace, as in the spec's example "mode_refval: 0 15625".
    YAML::Node refval = p.required(n, "mode_refval", what);
    {
        std::istringstream is(p.scalar(refval, what + ".mode_refval"));
        std::string a, b, extra;
        if (!(is >> a >> b) || (is >> extra))
            p.fail(refval, what + ".mode_refval must be two numbers, \"<count> <time>\", "
                               "e.g. \"1 0\"; got '" + refval.Scalar() + "'");
        t.ref_count = p.parseUint(refval, a, what + ".mode_refval count");
        t.ref_time = p.parseUint(refval, b, what + ".mode_refval time");
    }

    t.arm_mode = p.toEnum<PcmArmMode>(p.required(n, "arm_mode", what), what + ".arm_mode",
                                      {{"disarmed", PcmArmMode::DISARMED},
                                       {"once", PcmArmMode::ONCE},
                                       {"continuous", PcmArmMode::CONTINUOUS}});
    t.action = p.toEnum<PcmTriggerAction>(p.required(n, "action", what), what + ".action",
                                          {{"invoke", PcmTriggerAction::INVOKE},
                                           {"record", PcmTriggerAction::RECORD}});

    YAML::Node deadline = p.required(n, "invoke_deadline", what);
    t.invoke_deadline = p.toUint(deadline, what + ".invoke_deadline");
    if (t.action == PcmTriggerAction::RECORD && t.invoke_deadline != 0)
        spec.warnings.push_back(p.source() + ":" + std::to_string(Parser::lineOf(deadline)) +
                                ": " + what + ".invoke_deadline has no effect because "
                                "action is record (§3.6.18.5.1)");
    return t;
}

PcmSignalSpec parseSignal(const Parser& p, const YAML::Node& n, PcmSpec& spec) {
    std::string what = "signal entry";
    p.checkMap(n, what,
               {"name", "macro", "scope", "accumulator", "accumulator_const", "trigger"});
    PcmSignalSpec s;
    s.line = Parser::lineOf(n);

    YAML::Node name = p.required(n, "name", what);
    s.name = p.scalar(name, what + ".name");
    what = "signal '" + s.name + "'";
    s.info = pcmFindSignal(s.name);
    if (!s.info)
        p.fail(name, "unknown signal '" + s.name + "'");
    if (!s.info->supported)
        p.fail(name, "signal '" + s.name + "' is defined by the spec but not supported "
                     "by this simulator (supported signals: docs/pcm-plan.md §3)");
    if (s.info->vendor)
        spec.extensions_used.push_back("vendor signal " + s.name);

    s.macro = p.identifier(p.required(n, "macro", what), what + ".macro");
    what = "signal '" + s.name + "' (macro " + s.macro + ")";

    // `scope` is required by the YAML format (§3.6.18.8.5), but both of the
    // spec's signal examples (§3.6.18.5.5 and the timer example in §3.6.18.8.5)
    // leave it out. We let it be omitted, defaulting to CCC. EV scope is only for
    // load balancing, which this simulator doesn't support, so CCC is the only
    // value that is accepted anyway. See docs/pcm-plan.md §7.
    YAML::Node scope = n["scope"];
    if (scope.IsDefined()) {
        int sc = p.toEnum<int>(scope, what + ".scope", {{"ccc", 0}, {"ev", 1}});
        if (sc == 1)
            p.fail(scope, what + ": scope EV is only used by load-balancing algorithms, "
                              "which this simulator does not support");
    }

    // The YAML format marks `accumulator` as optional with a default from the
    // spec's tables, but those tables (3-110, 3-111) don't say which accumulator
    // is the default. We require it. Both spellings used in the spec are
    // accepted: uet_op_sum (Table 3-106) and uet_sum (the spec's examples).
    // See docs/pcm-plan.md §7.
    s.accumulator = p.toEnum<PcmAccOp>(p.required(n, "accumulator", what),
                                       what + ".accumulator",
                                       {{"uet_op_sum", PcmAccOp::SUM},
                                        {"uet_op_max", PcmAccOp::MAX},
                                        {"uet_op_min", PcmAccOp::MIN},
                                        {"uet_op_last", PcmAccOp::LAST},
                                        {"uet_op_ewma", PcmAccOp::EWMA},
                                        {"uet_sum", PcmAccOp::SUM},
                                        {"uet_max", PcmAccOp::MAX},
                                        {"uet_min", PcmAccOp::MIN},
                                        {"uet_last", PcmAccOp::LAST},
                                        {"uet_ewma", PcmAccOp::EWMA}});

    YAML::Node acc_const = n["accumulator_const"];
    if (s.accumulator == PcmAccOp::EWMA) {
        s.accumulator_const = p.toFloat(p.required(n, "accumulator_const", what),
                                        what + ".accumulator_const");
        if (!(s.accumulator_const > 0 && s.accumulator_const <= 1))
            p.fail(acc_const, what + ".accumulator_const (EWMA smoothing factor s) must "
                                     "be greater than 0 and at most 1");
    } else if (acc_const.IsDefined()) {
        p.fail(acc_const, what + ".accumulator_const is only used with uet_op_ewma");
    }

    YAML::Node trig = n["trigger"];
    if (trig.IsDefined())
        s.trigger = parseTrigger(p, trig, what + " trigger", spec);
    return s;
}

PcmControlSpec parseControl(const Parser& p, const YAML::Node& n) {
    std::string what = "control entry";
    p.checkMap(n, what, {"name", "macro", "initial_value"});
    PcmControlSpec c;
    c.line = Parser::lineOf(n);

    YAML::Node name = p.required(n, "name", what);
    c.name = p.scalar(name, what + ".name");

    // The spec's YAML format names controls from a table that isn't given
    // ("see Table ZZZ"). Each control function in Tables 3-118 and 3-119 has a
    // signal in Table 3-109 for the same quantity, so we use those signal names
    // as control names, e.g. uet_set_cwnd() sets the control uet_tx_cwnd.
    // See docs/pcm-plan.md §7.
    static const std::set<std::string> supported = {"uet_tx_cwnd"};
    static const std::set<std::string> later = {"uet_tx_send_rate", "uet_tx_ack_per_packet",
                                                "uet_rx_ack_gen_bytes", "uet_rx_ack_gen_timer",
                                                "uet_rx_cwnd_penalty"};
    if (later.count(c.name))
        p.fail(name, "control '" + c.name + "' is an optional control in the spec but is "
                     "not supported by this simulator yet; only uet_tx_cwnd is supported");
    if (!supported.count(c.name))
        p.fail(name, "unknown control '" + c.name + "' (supported: uet_tx_cwnd)");
    what = "control '" + c.name + "'";

    c.macro = p.identifier(p.required(n, "macro", what), what + ".macro");
    c.initial_value = p.toUint(p.required(n, "initial_value", what), what + ".initial_value");
    return c;
}

PcmVariableSpec parseVariable(const Parser& p, const YAML::Node& n) {
    std::string what = "variable entry";
    p.checkMap(n, what, {"macro", "dtype", "initial_value"});
    PcmVariableSpec v;
    v.line = Parser::lineOf(n);
    v.macro = p.identifier(p.required(n, "macro", what), what + ".macro");
    what = "variable " + v.macro;
    v.dtype = p.toEnum<PcmDtype>(p.required(n, "dtype", what), what + ".dtype", kDtypes);
    v.initial_value =
        parseValue(p, p.required(n, "initial_value", what), v.dtype, what + ".initial_value");
    return v;
}

PcmParameterSpec parseParameter(const Parser& p, const YAML::Node& n) {
    std::string what = "parameter entry";
    p.checkMap(n, what, {"macro", "dtype", "value", "description"});
    PcmParameterSpec par;
    par.line = Parser::lineOf(n);
    par.macro = p.identifier(p.required(n, "macro", what), what + ".macro");
    what = "parameter " + par.macro;
    par.dtype = p.toEnum<PcmDtype>(p.required(n, "dtype", what), what + ".dtype", kDtypes);
    par.value = parseValue(p, p.required(n, "value", what), par.dtype, what + ".value");
    par.description = p.scalar(p.required(n, "description", what), what + ".description");
    return par;
}

}  // namespace

// ---------------------------------------------------------------------------

const char* pcmAccOpName(PcmAccOp op) {
    switch (op) {
    case PcmAccOp::SUM: return "uet_op_sum";
    case PcmAccOp::MAX: return "uet_op_max";
    case PcmAccOp::MIN: return "uet_op_min";
    case PcmAccOp::LAST: return "uet_op_last";
    case PcmAccOp::EWMA: return "uet_op_ewma";
    }
    return "?";
}

const char* pcmTriggerOpName(PcmTriggerOp op) {
    switch (op) {
    case PcmTriggerOp::EQ: return "eq";
    case PcmTriggerOp::NEQ: return "neq";
    case PcmTriggerOp::GT: return "gt";
    case PcmTriggerOp::GTE: return "gte";
    case PcmTriggerOp::LT: return "lt";
    case PcmTriggerOp::LTE: return "lte";
    case PcmTriggerOp::WC: return "wc";
    }
    return "?";
}

PcmSpec pcmParseSpec(const std::string& yaml_text, const std::string& source_name) {
    Parser p(source_name);
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        std::ostringstream os;
        os << source_name;
        if (e.mark.line >= 0)
            os << ":" << e.mark.line + 1;
        os << ": YAML syntax error: " << e.msg;
        throw PcmSpecError(os.str());
    }
    if (!root.IsDefined() || root.IsNull())
        p.failTop("file is empty");

    PcmSpec spec;
    spec.source = source_name;

    try {
        if (!root.IsMap())
            p.fail(root, "the top level must be a mapping with keys cc_algorithm, signal, "
                         "control, and optionally variable and parameter");
        std::set<std::string> seen;
        for (auto it = root.begin(); it != root.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            if (!seen.insert(key).second)
                p.fail(it->first, "top-level key '" + key + "' appears twice");
            if (key == "global_variable")
                p.fail(it->first, "global_variable is not supported by this simulator yet "
                                  "(docs/pcm-plan.md §7)");
            if (key == "tgm")
                p.fail(it->first, "telegrams (tgm) are not supported by this simulator");
            if (key == "lb_algorithm")
                p.fail(it->first, "load-balancing algorithms (lb_algorithm) are not "
                                  "supported by this simulator");
            static const std::set<std::string> known = {"cc_algorithm", "signal", "control",
                                                        "variable", "parameter"};
            if (!known.count(key))
                p.fail(it->first, "unknown top-level key '" + key + "' (allowed: "
                                  "cc_algorithm, signal, control, variable, parameter)");
        }

        if (!root["cc_algorithm"].IsDefined())
            p.failTop("missing required top-level key 'cc_algorithm'");
        if (!root["signal"].IsDefined())
            p.failTop("missing required top-level key 'signal'");
        if (!root["control"].IsDefined())
            p.failTop("missing required top-level key 'control'");

        spec.algorithm = parseAlgorithm(p, root["cc_algorithm"]);
        for (const auto& e : p.entries(root["signal"], "signal"))
            spec.signals.push_back(parseSignal(p, e, spec));
        for (const auto& e : p.entries(root["control"], "control"))
            spec.controls.push_back(parseControl(p, e));
        if (root["variable"].IsDefined())
            for (const auto& e : p.entries(root["variable"], "variable"))
                spec.variables.push_back(parseVariable(p, e));
        if (root["parameter"].IsDefined())
            for (const auto& e : p.entries(root["parameter"], "parameter"))
                spec.parameters.push_back(parseParameter(p, e));
    } catch (const YAML::Exception& e) {
        // e.g. a key that is itself a list. Our own checks report everything else.
        std::ostringstream os;
        os << source_name;
        if (e.mark.line >= 0)
            os << ":" << e.mark.line + 1;
        os << ": " << e.msg;
        throw PcmSpecError(os.str());
    }

    // Macros name signals, controls, variables and parameters in the handler
    // code, so each must be unique in the file. The same signal may appear more
    // than once (one entry per accumulator, §3.6.18.4.1), but each entry needs
    // its own macro.
    std::map<std::string, std::string> macros;  // macro -> where it was defined
    auto addMacro = [&](const std::string& m, const std::string& where, int line) {
        auto [it, inserted] = macros.emplace(m, where);
        if (!inserted)
            throw PcmSpecError(source_name + ":" + std::to_string(line) + ": macro '" + m +
                               "' is used by both " + it->second + " and " + where);
    };
    for (const auto& s : spec.signals)
        addMacro(s.macro, "signal " + s.name + " (line " + std::to_string(s.line) + ")", s.line);
    std::set<std::string> control_names;
    for (const auto& c : spec.controls) {
        if (!control_names.insert(c.name).second)
            throw PcmSpecError(source_name + ":" + std::to_string(c.line) + ": control '" +
                               c.name + "' is listed twice");
        addMacro(c.macro, "control " + c.name + " (line " + std::to_string(c.line) + ")",
                 c.line);
    }
    for (const auto& v : spec.variables)
        addMacro(v.macro, "a variable (line " + std::to_string(v.line) + ")", v.line);
    for (const auto& par : spec.parameters)
        addMacro(par.macro, "a parameter (line " + std::to_string(par.line) + ")", par.line);

    bool can_invoke = false;
    for (const auto& s : spec.signals)
        if (s.trigger && s.trigger->action == PcmTriggerAction::INVOKE &&
            s.trigger->arm_mode != PcmArmMode::DISARMED)
            can_invoke = true;
    if (!can_invoke)
        spec.warnings.push_back(source_name + ": no trigger has action invoke and is armed, "
                                "so the handler will never be called");
    return spec;
}

PcmSpec pcmLoadSpecFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw PcmSpecError(path + ": cannot open file");
    std::stringstream buf;
    buf << in.rdbuf();
    return pcmParseSpec(buf.str(), path);
}
