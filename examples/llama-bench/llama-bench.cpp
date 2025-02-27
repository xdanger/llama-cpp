#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>

#include "ggml.h"
#include "llama.h"
#include "common.h"
#include "build-info.h"
#include "ggml-cuda.h"

// utils
static uint64_t get_time_ns() {
    using clock = std::chrono::high_resolution_clock;
    return std::chrono::nanoseconds(clock::now().time_since_epoch()).count();
}

template<class T>
static std::string join(const std::vector<T> & values, const std::string & delim) {
    std::ostringstream str;
    for (size_t i = 0; i < values.size(); i++) {
        str << values[i];
        if (i < values.size() - 1) {
            str << delim;
        }
    }
    return str.str();
}

template<class T>
static std::vector<T> split(const std::string & str, char delim) {
    std::vector<T> values;
    std::istringstream str_stream(str);
    std::string token;
    while (std::getline(str_stream, token, delim)) {
        T value;
        std::istringstream token_stream(token);
        token_stream >> value;
        values.push_back(value);
    }
    return values;
}

template<typename T>
static T avg(const std::vector<T> & v) {
    if (v.empty()) {
        return 0;
    }
    T sum = std::accumulate(v.begin(), v.end(), T(0));
    return sum / (T)v.size();
}

template<typename T>
static T stdev(const std::vector<T> & v) {
    if (v.size() <= 1) {
        return 0;
    }
    T mean = avg(v);
    T sq_sum = std::inner_product(v.begin(), v.end(), v.begin(), T(0));
    T stdev = std::sqrt(sq_sum / (T)(v.size() - 1) - mean * mean * (T)v.size() / (T)(v.size() - 1));
    return stdev;
}

static bool ggml_cpu_has_metal() {
#if defined(GGML_USE_METAL)
    return true;
#else
    return false;
#endif
}

static std::string get_cpu_info() {
    std::string id;
#ifdef __linux__
    FILE * f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "model name", 10) == 0) {
                char * p = strchr(buf, ':');
                if (p) {
                    p++;
                    while (std::isspace(*p)) {
                        p++;
                    }
                    while (std::isspace(p[strlen(p) - 1])) {
                        p[strlen(p) - 1] = '\0';
                    }
                    id = p;
                    break;
                }
            }
        }
    }
#endif
    // TODO: other platforms
    return id;
}

static std::string get_gpu_info() {
    std::string id;
#ifdef GGML_USE_CUBLAS
    int count = ggml_cuda_get_device_count();
    for (int i = 0; i < count; i++) {
        char buf[128];
        ggml_cuda_get_device_description(i, buf, sizeof(buf));
        id += buf;
        if (i < count - 1) {
            id += "/";
        }
    }
#endif
    // TODO: other backends
    return id;
}

// command line params
enum output_formats {CSV, JSON, MARKDOWN, SQL};

struct cmd_params {
    std::vector<std::string> model;
    std::vector<int> n_prompt;
    std::vector<int> n_gen;
    std::vector<int> n_batch;
    std::vector<bool> f32_kv;
    std::vector<int> n_threads;
    std::vector<int> n_gpu_layers;
    std::vector<int> main_gpu;
    std::vector<bool> mul_mat_q;
    std::vector<bool> low_vram;
    std::vector<std::array<float, LLAMA_MAX_DEVICES>> tensor_split;
    int reps;
    bool verbose;
    output_formats output_format;
};

static const cmd_params cmd_params_defaults = {
    /* model         */ {"models/7B/ggml-model-q4_0.gguf"},
    /* n_prompt      */ {512},
    /* n_gen         */ {128},
    /* n_batch       */ {512},
    /* f32_kv        */ {false},
    /* n_threads     */ {get_num_physical_cores()},
    /* n_gpu_layers  */ {99},
    /* main_gpu      */ {0},
    /* mul_mat_q     */ {true},
    /* low_vram      */ {false},
    /* tensor_split  */ {{}},
    /* reps          */ 5,
    /* verbose       */ false,
    /* output_format */ MARKDOWN
};

static void print_usage(int /* argc */, char ** argv) {
    fprintf(stdout, "usage: %s [options]\n", argv[0]);
    fprintf(stdout, "\n");
    fprintf(stdout, "options:\n");
    fprintf(stdout, "  -h, --help\n");
    fprintf(stdout, "  -m, --model <filename>            (default: %s)\n", join(cmd_params_defaults.model, ",").c_str());
    fprintf(stdout, "  -p, --n-prompt <n>                (default: %s)\n", join(cmd_params_defaults.n_prompt, ",").c_str());
    fprintf(stdout, "  -n, --n-gen <n>                   (default: %s)\n", join(cmd_params_defaults.n_gen, ",").c_str());
    fprintf(stdout, "  -b, --batch-size <n>              (default: %s)\n", join(cmd_params_defaults.n_batch, ",").c_str());
    fprintf(stdout, "  --memory-f32 <0|1>                (default: %s)\n", join(cmd_params_defaults.f32_kv, ",").c_str());
    fprintf(stdout, "  -t, --threads <n>                 (default: %s)\n", join(cmd_params_defaults.n_threads, ",").c_str());
    fprintf(stdout, "  -ngl N, --n-gpu-layers <n>        (default: %s)\n", join(cmd_params_defaults.n_gpu_layers, ",").c_str());
    fprintf(stdout, "  -mg i, --main-gpu <n>             (default: %s)\n", join(cmd_params_defaults.main_gpu, ",").c_str());
    fprintf(stdout, "  -lv, --low-vram <0|1>             (default: %s)\n", join(cmd_params_defaults.low_vram, ",").c_str());
    fprintf(stdout, "  -mmq, --mul-mat-q <0|1>           (default: %s)\n", join(cmd_params_defaults.mul_mat_q, ",").c_str());
    fprintf(stdout, "  -ts, --tensor_split <ts0/ts1/..>               \n");
    fprintf(stdout, "  -r, --repetitions <n>             (default: %d)\n", cmd_params_defaults.reps);
    fprintf(stdout, "  -o, --output <csv|json|md|sql>    (default: %s)\n", cmd_params_defaults.output_format == CSV ? "csv" : cmd_params_defaults.output_format == JSON ? "json" : cmd_params_defaults.output_format == MARKDOWN ? "md" : "sql");
    fprintf(stdout, "  -v, --verbose                     (default: %s)\n", cmd_params_defaults.verbose ? "1" : "0");
    fprintf(stdout, "\n");
    fprintf(stdout, "Multiple values can be given for each parameter by separating them with ',' or by specifying the parameter multiple times.\n");

}

static cmd_params parse_cmd_params(int argc, char ** argv) {
    cmd_params params;
    std::string arg;
    bool invalid_param = false;
    const std::string arg_prefix = "--";
    const char split_delim = ',';

    params.verbose = cmd_params_defaults.verbose;
    params.output_format = cmd_params_defaults.output_format;
    params.reps = cmd_params_defaults.reps;

    for (int i = 1; i < argc; i++) {
        arg = argv[i];
        if (arg.compare(0, arg_prefix.size(), arg_prefix) == 0) {
            std::replace(arg.begin(), arg.end(), '_', '-');
        }

        if (arg == "-h" || arg == "--help") {
            print_usage(argc, argv);
            exit(0);
        } else if (arg == "-m" || arg == "--model") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<std::string>(argv[i], split_delim);
            params.model.insert(params.model.end(), p.begin(), p.end());
        } else if (arg == "-p" || arg == "--n-prompt") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.n_prompt.insert(params.n_prompt.end(), p.begin(), p.end());
        } else if (arg == "-n" || arg == "--n-gen") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.n_gen.insert(params.n_gen.end(), p.begin(), p.end());
        } else if (arg == "-b" || arg == "--batch-size") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.n_batch.insert(params.n_batch.end(), p.begin(), p.end());
        } else if (arg == "--memory-f32") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.f32_kv.insert(params.f32_kv.end(), p.begin(), p.end());
        } else if (arg == "-t" || arg == "--threads") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.n_threads.insert(params.n_threads.end(), p.begin(), p.end());
        } else if (arg == "-ngl" || arg == "--n-gpu-layers") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<int>(argv[i], split_delim);
            params.n_gpu_layers.insert(params.n_gpu_layers.end(), p.begin(), p.end());
        } else if (arg == "-mg" || arg == "--main-gpu") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            params.main_gpu = split<int>(argv[i], split_delim);
        } else if (arg == "-lv" || arg == "--low-vram") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<bool>(argv[i], split_delim);
            params.low_vram.insert(params.low_vram.end(), p.begin(), p.end());
        } else if (arg == "-mmq" || arg == "--mul-mat-q") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            auto p = split<bool>(argv[i], split_delim);
            params.mul_mat_q.insert(params.mul_mat_q.end(), p.begin(), p.end());
        } else if (arg == "-ts" || arg == "--tensor-split") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            for (auto ts : split<std::string>(argv[i], split_delim)) {
                // split string by ; and /
                const std::regex regex{R"([;/]+)"};
                std::sregex_token_iterator it{ts.begin(), ts.end(), regex, -1};
                std::vector<std::string> split_arg{it, {}};
                GGML_ASSERT(split_arg.size() <= LLAMA_MAX_DEVICES);

                std::array<float, LLAMA_MAX_DEVICES> tensor_split;
                for (size_t i = 0; i < LLAMA_MAX_DEVICES; ++i) {
                    if (i < split_arg.size()) {
                        tensor_split[i] = std::stof(split_arg[i]);
                    } else {
                        tensor_split[i] = 0.0f;
                    }
                }
                params.tensor_split.push_back(tensor_split);
            }
        } else if (arg == "-r" || arg == "--repetitions") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            params.reps = std::stoi(argv[i]);
        } else if (arg == "-o" || arg == "--output") {
            if (++i >= argc) {
                invalid_param = true;
                break;
            }
            if (argv[i] == std::string("csv")) {
                params.output_format = CSV;
            } else if (argv[i] == std::string("json")) {
                params.output_format = JSON;
            } else if (argv[i] == std::string("md")) {
                params.output_format = MARKDOWN;
            } else if (argv[i] == std::string("sql")) {
                params.output_format = SQL;
            } else {
                invalid_param = true;
                break;
            }
        } else if (arg == "-v" || arg == "--verbose") {
            params.verbose = true;
        } else {
            invalid_param = true;
            break;
        }
    }
    if (invalid_param) {
        fprintf(stderr, "error: invalid parameter for argument: %s\n", arg.c_str());
        print_usage(argc, argv);
        exit(1);
    }

    // set defaults
    if (params.model.empty())        { params.model = cmd_params_defaults.model; }
    if (params.n_prompt.empty())     { params.n_prompt = cmd_params_defaults.n_prompt; }
    if (params.n_gen.empty())        { params.n_gen = cmd_params_defaults.n_gen; }
    if (params.n_batch.empty())      { params.n_batch = cmd_params_defaults.n_batch; }
    if (params.f32_kv.empty())       { params.f32_kv = cmd_params_defaults.f32_kv; }
    if (params.n_gpu_layers.empty()) { params.n_gpu_layers = cmd_params_defaults.n_gpu_layers; }
    if (params.main_gpu.empty())     { params.main_gpu = cmd_params_defaults.main_gpu; }
    if (params.mul_mat_q.empty())    { params.mul_mat_q = cmd_params_defaults.mul_mat_q; }
    if (params.low_vram.empty())     { params.low_vram = cmd_params_defaults.low_vram; }
    if (params.tensor_split.empty()) { params.tensor_split = cmd_params_defaults.tensor_split; }
    if (params.n_threads.empty())    { params.n_threads = cmd_params_defaults.n_threads; }

    return params;
}

struct cmd_params_instance {
    std::string model;
    int n_prompt;
    int n_gen;
    int n_batch;
    bool f32_kv;
    int n_threads;
    int n_gpu_layers;
    int main_gpu;
    bool mul_mat_q;
    bool low_vram;
    std::array<float, LLAMA_MAX_DEVICES> tensor_split;

    llama_context_params to_llama_params() const {
        llama_context_params lparams = llama_context_default_params();
        lparams.n_ctx = n_prompt + n_gen;
        lparams.n_batch = n_batch;
        lparams.f16_kv = !f32_kv;
        lparams.n_gpu_layers = n_gpu_layers;
        lparams.main_gpu = main_gpu;
        lparams.mul_mat_q = mul_mat_q;
        lparams.low_vram = low_vram;
        lparams.tensor_split = tensor_split.data();

        return lparams;
    }
};

static std::vector<cmd_params_instance> get_cmd_params_instances_int(const cmd_params & params, int n_gen, int n_prompt) {
    std::vector<cmd_params_instance> instances;

    for (const auto & m : params.model)
    for (const auto & nb : params.n_batch)
    for (const auto & fk : params.f32_kv)
    for (const auto & nl : params.n_gpu_layers)
    for (const auto & mg : params.main_gpu)
    for (const auto & mmq : params.mul_mat_q)
    for (const auto & lv : params.low_vram)
    for (const auto & ts : params.tensor_split)
    for (const auto & nt : params.n_threads) {
        cmd_params_instance instance = {
            /* .model        = */ m,
            /* .n_prompt     = */ n_prompt,
            /* .n_gen        = */ n_gen,
            /* .n_batch      = */ nb,
            /* .f32_kv       = */ fk,
            /* .n_threads    = */ nt,
            /* .n_gpu_layers = */ nl,
            /* .main_gpu     = */ mg,
            /* .mul_mat_q    = */ mmq,
            /* .low_vram     = */ lv,
            /* .tensor_split = */ ts,
        };
        instances.push_back(instance);
    }
    return instances;
}

static std::vector<cmd_params_instance> get_cmd_params_instances(const cmd_params & params) {
    std::vector<cmd_params_instance> instances;

    for (const auto & n_prompt : params.n_prompt) {
        if (n_prompt == 0) {
            continue;
        }
        auto instances_prompt = get_cmd_params_instances_int(params, 0, n_prompt);
        instances.insert(instances.end(), instances_prompt.begin(), instances_prompt.end());
    }

    for (const auto & n_gen : params.n_gen) {
        if (n_gen == 0) {
            continue;
        }
        auto instances_gen = get_cmd_params_instances_int(params, n_gen, 0);
        instances.insert(instances.end(), instances_gen.begin(), instances_gen.end());
    }

    return instances;
}

struct test {
    static const std::string build_commit;
    static const int build_number;
    static const bool cuda;
    static const bool opencl;
    static const bool metal;
    static const bool gpu_blas;
    static const bool blas;
    static const std::string cpu_info;
    static const std::string gpu_info;
    std::string model_filename;
    std::string model_type;
    int n_batch;
    int n_threads;
    bool f32_kv;
    int n_gpu_layers;
    int main_gpu;
    bool mul_mat_q;
    bool low_vram;
    std::array<float, LLAMA_MAX_DEVICES> tensor_split;
    int n_prompt;
    int n_gen;
    std::string test_time;
    std::vector<uint64_t> samples_ns;

    test(const cmd_params_instance & inst, const llama_model * lmodel, const llama_context * ctx) {
        model_filename = inst.model;
        char buf[128];
        llama_model_type(lmodel, buf, sizeof(buf));
        model_type = buf;
        n_batch = inst.n_batch;
        n_threads = inst.n_threads;
        f32_kv = inst.f32_kv;
        n_gpu_layers = inst.n_gpu_layers;
        main_gpu = inst.main_gpu;
        mul_mat_q = inst.mul_mat_q;
        low_vram = inst.low_vram;
        tensor_split = inst.tensor_split;
        n_prompt = inst.n_prompt;
        n_gen = inst.n_gen;
        // RFC 3339 date-time format
        time_t t = time(NULL);
        std::strftime(buf, sizeof(buf), "%FT%TZ", gmtime(&t));
        test_time = buf;

        (void) ctx;
    }

    uint64_t avg_ns() const {
        return ::avg(samples_ns);
    }

    uint64_t stdev_ns() const {
        return ::stdev(samples_ns);
    }

    std::vector<double> get_ts() const {
        int n_tokens = n_prompt + n_gen;
        std::vector<double> ts;
        std::transform(samples_ns.begin(), samples_ns.end(), std::back_inserter(ts), [n_tokens](uint64_t t) { return 1e9 * n_tokens / t; });
        return ts;
    }

    double avg_ts() const {
        return ::avg(get_ts());
    }

    double stdev_ts() const {
        return ::stdev(get_ts());
    }

    static std::string get_backend() {
        if (cuda) {
            return GGML_CUDA_NAME;
        }
        if (opencl) {
            return "OpenCL";
        }
        if (metal) {
            return "Metal";
        }
        if (gpu_blas) {
            return "GPU BLAS";
        }
        if (blas) {
            return "BLAS";
        }
        return "CPU";
    }

    static const std::vector<std::string> & get_fields() {
        static const std::vector<std::string> fields = {
            "build_commit", "build_number",
            "cuda", "opencl", "metal", "gpu_blas", "blas",
            "cpu_info", "gpu_info",
            "model_filename", "model_type",
            "n_batch", "n_threads", "f16_kv",
            "n_gpu_layers", "main_gpu", "mul_mat_q", "low_vram", "tensor_split",
            "n_prompt", "n_gen", "test_time",
            "avg_ns", "stddev_ns",
            "avg_ts", "stddev_ts"
        };
        return fields;
    }

    enum field_type {STRING, BOOL, INT, FLOAT};

    static field_type get_field_type(const std::string & field) {
        if (field == "build_number" || field == "n_batch" || field == "n_threads" ||
            field == "n_gpu_layers" || field == "main_gpu" ||
            field == "n_prompt" || field == "n_gen" ||
            field == "avg_ns" || field == "stddev_ns") {
            return INT;
        }
        if (field == "cuda" || field == "opencl" || field == "metal" || field == "gpu_blas" || field == "blas" ||
            field == "f16_kv" || field == "mul_mat_q" || field == "low_vram") {
            return BOOL;
        }
        if (field == "avg_ts" || field == "stddev_ts") {
            return FLOAT;
        }
        return STRING;
    }

    std::vector<std::string> get_values() const {
        std::string tensor_split_str;
        int max_nonzero = 0;
        for (int i = 0; i < LLAMA_MAX_DEVICES; i++) {
            if (tensor_split[i] > 0) {
                max_nonzero = i;
            }
        }
        for (int i = 0; i <= max_nonzero; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", tensor_split[i]);
            tensor_split_str += buf;
            if (i < max_nonzero) {
                tensor_split_str += "/";
            }
        }
        std::vector<std::string> values = {
            build_commit, std::to_string(build_number),
            std::to_string(cuda), std::to_string(opencl), std::to_string(metal), std::to_string(gpu_blas), std::to_string(blas),
            cpu_info, gpu_info,
            model_filename, model_type,
            std::to_string(n_batch), std::to_string(n_threads), std::to_string(!f32_kv),
            std::to_string(n_gpu_layers), std::to_string(main_gpu), std::to_string(mul_mat_q), std::to_string(low_vram), tensor_split_str,
            std::to_string(n_prompt), std::to_string(n_gen), test_time,
            std::to_string(avg_ns()), std::to_string(stdev_ns()),
            std::to_string(avg_ts()), std::to_string(stdev_ts())
        };
        return values;
    }

    std::map<std::string, std::string> get_map() const {
        std::map<std::string, std::string> map;
        auto fields = get_fields();
        auto values = get_values();
        std::transform(fields.begin(), fields.end(), values.begin(),
                std::inserter(map, map.end()), std::make_pair<const std::string &, const std::string &>);
        return map;
    }
};

const std::string test::build_commit = BUILD_COMMIT;
const int         test::build_number = BUILD_NUMBER;
const bool        test::cuda         = !!ggml_cpu_has_cublas();
const bool        test::opencl       = !!ggml_cpu_has_clblast();
const bool        test::metal        = !!ggml_cpu_has_metal();
const bool        test::gpu_blas     = !!ggml_cpu_has_gpublas();
const bool        test::blas         = !!ggml_cpu_has_blas();
const std::string test::cpu_info     = get_cpu_info();
const std::string test::gpu_info     = get_gpu_info();

struct printer {
    virtual ~printer() {}

    FILE * fout;
    virtual void print_header(const cmd_params & params) { (void) params; };
    virtual void print_test(const test & t) = 0;
    virtual void print_footer() { };
};

struct csv_printer : public printer {
    static std::string escape_csv(const std::string & field) {
        std::string escaped = "\"";
        for (auto c : field) {
            if (c == '"') {
                escaped += "\"";
            }
            escaped += c;
        }
        escaped += "\"";
        return escaped;
    }

    void print_header(const cmd_params & params) override  {
        std::vector<std::string> fields = test::get_fields();
        fprintf(fout, "%s\n", join(fields, ",").c_str());
        (void) params;
    }

    void print_test(const test & t) override {
        std::vector<std::string> values = t.get_values();
        std::transform(values.begin(), values.end(), values.begin(), escape_csv);
        fprintf(fout, "%s\n", join(values, ",").c_str());
    }
};

struct json_printer : public printer {
    bool first = true;

    static std::string escape_json(const std::string & value) {
        std::string escaped;
        for (auto c : value) {
            if (c == '"') {
                escaped += "\\\"";
            } else if (c == '\\') {
                escaped += "\\\\";
            } else  if (c <= 0x1f) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                escaped += buf;
            } else {
                escaped += c;
            }
        }
        return escaped;
    }

    static std::string format_value(const std::string & field, const std::string & value) {
        switch (test::get_field_type(field)) {
            case test::STRING:
                return "\"" + escape_json(value) + "\"";
            case test::BOOL:
                return value == "0" ? "false" : "true";
            default:
                return value;
        }
    }

    void print_header(const cmd_params & params) override {
        fprintf(fout, "[\n");
        (void) params;
    }

    void print_fields(const std::vector<std::string> & fields, const std::vector<std::string> & values) {
        assert(fields.size() == values.size());
        for (size_t i = 0; i < fields.size(); i++) {
            fprintf(fout, "    \"%s\": %s,\n", fields.at(i).c_str(), format_value(fields.at(i), values.at(i)).c_str());
        }
    }

    void print_test(const test & t) override {
        if (first) {
            first = false;
        } else {
            fprintf(fout, ",\n");
        }
        fprintf(fout, "  {\n");
        print_fields(test::get_fields(), t.get_values());
        fprintf(fout, "    \"samples_ns\": [ %s ],\n", join(t.samples_ns, ", ").c_str());
        fprintf(fout, "    \"samples_ts\": [ %s ]\n", join(t.get_ts(), ", ").c_str());
        fprintf(fout, "  }");
        fflush(fout);
    }

    void print_footer() override {
        fprintf(fout, "\n]\n");
    }
};

struct markdown_printer : public printer {
    std::vector<std::string> fields;

    static int get_field_width(const std::string & field) {
        if (field == "model") {
            return -30;
        }
        if (field == "t/s") {
            return 15;
        }
        int width = std::max((int)field.length(), 10);

        if (test::get_field_type(field) == test::STRING) {
            return -width;
        }
        return width;
    }

    void print_header(const cmd_params & params) override {
        // select fields to print
        fields = { "model", "backend" };
        bool is_cpu_backend = test::get_backend() == "CPU" || test::get_backend() == "BLAS";
        if (!is_cpu_backend) {
            fields.push_back("n_gpu_layers");
        }
        if (params.n_threads.size() > 1 || params.n_threads != cmd_params_defaults.n_threads || is_cpu_backend) {
            fields.push_back("n_threads");
        }
        if (params.n_batch.size() > 1 || params.n_batch != cmd_params_defaults.n_batch) {
            fields.push_back("n_batch");
        }
        if (params.f32_kv.size() > 1 || params.f32_kv != cmd_params_defaults.f32_kv) {
            fields.push_back("f16_kv");
        }
        if (params.main_gpu.size() > 1 || params.main_gpu != cmd_params_defaults.main_gpu) {
            fields.push_back("main_gpu");
        }
        if (params.mul_mat_q.size() > 1 || params.mul_mat_q != cmd_params_defaults.mul_mat_q) {
            fields.push_back("mul_mat_q");
        }
        if (params.low_vram.size() > 1 || params.low_vram != cmd_params_defaults.low_vram) {
            fields.push_back("low_vram");
        }
        if (params.tensor_split.size() > 1 || params.tensor_split != cmd_params_defaults.tensor_split) {
            fields.push_back("tensor_split");
        }
        fields.push_back("test");
        fields.push_back("t/s");

        fprintf(fout, "|");
        for (const auto & field : fields) {
            fprintf(fout, " %*s |", get_field_width(field), field.c_str());
        }
        fprintf(fout, "\n");
        fprintf(fout, "|");
        for (const auto & field : fields) {
            int width = get_field_width(field);
            fprintf(fout, " %s%s |", std::string(std::abs(width) - 1, '-').c_str(), width > 0 ? ":" : "-");
        }
        fprintf(fout, "\n");
    }

    void print_test(const test & t) override {
        std::map<std::string, std::string> vmap = t.get_map();

        fprintf(fout, "|");
        for (const auto & field : fields) {
            std::string value;
            if (field == "model") {
                value = t.model_type;
            } else if (field == "backend") {
                value = test::get_backend();
            } else if (field == "test") {
                char buf[128];
                if (t.n_prompt > 0 && t.n_gen == 0) {
                    snprintf(buf, sizeof(buf), "pp %d", t.n_prompt);
                } else if (t.n_gen > 0 && t.n_prompt == 0) {
                    snprintf(buf, sizeof(buf), "tg %d", t.n_gen);
                } else {
                    assert(false);
                    exit(1);
                }
                value = buf;
            } else if (field == "t/s") {
                char buf[128];
                snprintf(buf, sizeof(buf), "%.2f ± %.2f", t.avg_ts(), t.stdev_ts());
                value = buf;
            } else if (vmap.find(field) != vmap.end()) {
                value = vmap.at(field);
            } else {
                assert(false);
                exit(1);
            }

            int width = get_field_width(field);
            if (field == "t/s") {
                // HACK: the utf-8 character is 2 bytes
                width += 1;
            }
            fprintf(fout, " %*s |", width, value.c_str());
        }
        fprintf(fout, "\n");
    }

    void print_footer() override {
        fprintf(fout, "\nbuild: %s (%d)\n", test::build_commit.c_str(), test::build_number);
    }
};

struct sql_printer : public printer {
    static std::string get_sql_field_type(const std::string & field) {
        switch (test::get_field_type(field)) {
            case test::STRING:
                return "TEXT";
            case test::BOOL:
            case test::INT:
                return "INTEGER";
            case test::FLOAT:
                return "REAL";
            default:
                assert(false);
                exit(1);
        }
    }

    void print_header(const cmd_params & params) override {
        std::vector<std::string> fields = test::get_fields();
        fprintf(fout, "CREATE TABLE IF NOT EXISTS test (\n");
        for (size_t i = 0; i < fields.size(); i++) {
            fprintf(fout, "  %s %s%s\n", fields.at(i).c_str(), get_sql_field_type(fields.at(i)).c_str(),  i < fields.size() - 1 ? "," : "");
        }
        fprintf(fout, ");\n");
        fprintf(fout, "\n");
        (void) params;
    }

    void print_test(const test & t) override {
        fprintf(fout, "INSERT INTO test (%s) ", join(test::get_fields(), ", ").c_str());
        fprintf(fout, "VALUES (");
        std::vector<std::string> values = t.get_values();
        for (size_t i = 0; i < values.size(); i++) {
            fprintf(fout, "'%s'%s", values.at(i).c_str(), i < values.size() - 1 ? ", " : "");
        }
        fprintf(fout, ");\n");
    }
};

static void test_prompt(llama_context * ctx, int n_prompt, int n_past, int n_batch, int n_threads) {
    std::vector<llama_token> tokens(n_batch, llama_token_bos(ctx));
    int n_processed = 0;
    while (n_processed < n_prompt) {
        int n_tokens = std::min(n_prompt - n_processed, n_batch);
        llama_eval(ctx, tokens.data(), n_tokens, n_past + n_processed, n_threads);
        n_processed += n_tokens;
    }
}

static void test_gen(llama_context * ctx, int n_gen, int n_past, int n_threads) {
    llama_token token = llama_token_bos(ctx);
    for (int i = 0; i < n_gen; i++) {
        llama_eval(ctx, &token, 1, n_past + i, n_threads);
    }
}

static void llama_null_log_callback(enum llama_log_level level, const char * text, void * user_data) {
    (void) level;
    (void) text;
    (void) user_data;
}

int main(int argc, char ** argv) {
#if !defined(NDEBUG)
    fprintf(stderr, "warning: asserts enabled, performance may be affected\n");
#endif

#if (defined(_MSC_VER) && defined(_DEBUG)) || (!defined(_MSC_VER) && !defined(__OPTIMIZE__))
    fprintf(stderr, "warning: debug build, performance may be affected\n");
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    fprintf(stderr, "warning: sanitizer enabled, performance may be affected\n");
#endif

    cmd_params params = parse_cmd_params(argc, argv);

    // initialize llama.cpp
    if (!params.verbose) {
        llama_log_set(llama_null_log_callback, NULL);
    }
    bool numa = false;
    llama_backend_init(numa);

    // initialize printer
    std::unique_ptr<printer> p;
    switch (params.output_format) {
        case CSV:
            p.reset(new csv_printer());
            break;
        case JSON:
            p.reset(new json_printer());
            break;
        case MARKDOWN:
            p.reset(new markdown_printer());
            break;
        case SQL:
            p.reset(new sql_printer());
            break;
        default:
            assert(false);
            exit(1);
    }
    p->fout = stdout;
    p->print_header(params);

    std::vector<cmd_params_instance> params_instances = get_cmd_params_instances(params);

    for (const auto & inst : params_instances) {
        // TODO: keep the model between tests when possible
        llama_context_params lparams = inst.to_llama_params();

        llama_model * lmodel  = llama_load_model_from_file(inst.model.c_str(), lparams);
        if (lmodel == NULL) {
            fprintf(stderr, "%s: error: failed to load model '%s'\n", __func__, inst.model.c_str());
            return 1;
        }

        llama_context * ctx = llama_new_context_with_model(lmodel, lparams);
        if (ctx == NULL) {
            fprintf(stderr, "%s: error: failed to create context with model '%s'\n", __func__, inst.model.c_str());
            llama_free_model(lmodel);
            return 1;
        }

        test t(inst, lmodel, ctx);

        // warmup run
        test_gen(ctx, 1, 0, t.n_threads);

        for (int i = 0; i < params.reps; i++) {
            uint64_t t_start = get_time_ns();
            if (t.n_prompt > 0) {
                test_prompt(ctx, t.n_prompt, 0, t.n_batch, t.n_threads);
            }
            if (t.n_gen > 0) {
                test_gen(ctx, t.n_gen, t.n_prompt, t.n_threads);
            }
            uint64_t t_ns = get_time_ns() - t_start;
            t.samples_ns.push_back(t_ns);
        }

        p->print_test(t);

        llama_print_timings(ctx);

        llama_free(ctx);
        llama_free_model(lmodel);
    }

    p->print_footer();

    llama_backend_free();

    return 0;
}
