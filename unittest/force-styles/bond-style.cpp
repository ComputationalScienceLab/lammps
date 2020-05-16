// unit tests for bond styles intended for molecular systems

#include "lammps.h"
#include "atom.h"
#include "modify.h"
#include "compute.h"
#include "force.h"
#include "bond.h"
#include "info.h"
#include "input.h"
#include "universe.h"

#include <mpi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <ctime>

#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "yaml.h"

using ::testing::StartsWith;
using ::testing::HasSubstr;

struct coord_t {
    double x,y,z;
};

struct stress_t {
    double xx,yy,zz,xy,xz,yz;
};

class TestConfig {
public:
    std::string lammps_version;
    std::string date_generated;
    double epsilon;
    std::vector<std::pair<std::string,std::string>> prerequisites;
    std::vector<std::string> pre_commands;
    std::vector<std::string> post_commands;
    std::string input_file;
    std::string bond_style;
    std::vector<std::string> bond_coeff;
    int natoms;
    double init_energy;
    double run_energy;
    stress_t init_stress;
    stress_t run_stress;
    std::vector<coord_t> init_forces;
    std::vector<coord_t> run_forces;
    TestConfig() : lammps_version(""),
                   date_generated(""),
                   epsilon(1.0e-14),
                   input_file(""),
                   bond_style("zero"),
                   natoms(0),
                   init_energy(0),
                   run_energy(0),
                   init_stress({0,0,0,0,0,0}),
                   run_stress({0,0,0,0,0,0}) {
        prerequisites.clear();
        pre_commands.clear();
        post_commands.clear();
        bond_coeff.clear();
        init_forces.clear();
        run_forces.clear();
    }
};

TestConfig test_config;

// whether to print error statistics
bool print_stats = false;

class ErrorStats
{
public:
    friend std::ostream &operator<<(std::ostream &out, const ErrorStats &stats);
    ErrorStats() {
            reset();
    }
    virtual ~ErrorStats() {}
    void reset() {
        num = 0;
        maxidx = -1;
        sum = sumsq = maxerr =0.0;
    }
    void add(const double &val) {
        ++num;
        if (val > maxerr) {
            maxidx = num;
            maxerr = val;
        }
        sum += val;
        sumsq += val*val;
    }
    double avg() const {
        return (num > 0) ? sum/num : 0.0;
    }
    double dev() const {
        return (num > 0) ? sqrt(sumsq/num - sum/num*sum/num) : 0.0;
    }
    double max() const { return maxerr; }
    double idx() const { return maxidx; }

private:
    double sum,sumsq,maxerr;
    int num,maxidx;
};

std::ostream &operator<<(std::ostream &out, const ErrorStats &stats)
{
    const std::ios_base::fmtflags flags = out.flags();
    const std::streamsize width = out.width(10);
    const std::streamsize prec = out.precision(3);

    out << std::scientific
        << "Average: " << stats.avg()
        << " StdDev: " << stats.dev()
        << " MaxErr: " << stats.max();

    out.precision(prec);
    out.width(width);
    out.flags(flags);

    return out << " @ item: " << stats.idx();
}

#define EXPECT_FP_LE_WITH_EPS(val1,val2,eps)                \
    do {                                                    \
        const double diff = fabs(val1-val2);                \
        const double div = std::min(fabs(val1),fabs(val2)); \
        const double err = (div == 0.0) ? diff : diff/div;  \
        stats.add(err);                                     \
        EXPECT_PRED_FORMAT2(::testing::DoubleLE, err, eps); \
    } while (0);

LAMMPS_NS::LAMMPS *init_lammps(int argc, char **argv, const TestConfig &cfg)
{
    LAMMPS_NS::LAMMPS *lmp;

    lmp = new LAMMPS_NS::LAMMPS(argc, argv, MPI_COMM_WORLD);

    // check if prerequisite styles are available
    LAMMPS_NS::Info *info = new LAMMPS_NS::Info(lmp);
    int nfail = 0;
    for (auto prerequisite : cfg.prerequisites) {
        std::string style = prerequisite.second;

        // this is a test for bond styles, so if the suffixed
        // version is not available, there is no reason to test.
        if (prerequisite.first == "bond") {
            if (lmp->suffix_enable) {
                style += "/";
                style += lmp->suffix;
            }
        }

        if (!info->has_style(prerequisite.first,style)) ++nfail;
    }
    if (nfail > 0) {
        delete info;
        delete lmp;
        return NULL;
    }

    for (auto pre_command : cfg.pre_commands)
        lmp->input->one(pre_command.c_str());
    lmp->input->file(cfg.input_file.c_str());

    std::string cmd("bond_style ");
    cmd += cfg.bond_style;
    lmp->input->one(cmd.c_str());
    for (auto bond_coeff : cfg.bond_coeff) {
        cmd = "bond_coeff " + bond_coeff;
        lmp->input->one(cmd.c_str());
    }
    for (auto post_command : cfg.post_commands)
        lmp->input->one(post_command.c_str());
    lmp->input->one("run 0 post no");
    return lmp;
}

void run_lammps(LAMMPS_NS::LAMMPS *lmp)
{
    lmp->input->one("fix 1 all nve");
    lmp->input->one("compute pe all pe/atom");
    lmp->input->one("compute sum all reduce sum c_pe");
    lmp->input->one("thermo_style custom step temp pe press c_sum");
    lmp->input->one("thermo 2");
    lmp->input->one("run 4 post no");
}

template<typename ConsumerClass>
class YamlReader {
    enum StateValue {
        START,
        ACCEPT_KEY,
        ACCEPT_VALUE,
        STOP,
        ERROR,
    };

    StateValue state;
    bool accepted;
    std::string key;

protected:
    typedef void (ConsumerClass::*EventConsumer)(const yaml_event_t & event);
    std::map<std::string, EventConsumer> consumers;

public:
    YamlReader() {
    }

    virtual ~YamlReader() {
    }

    int parse_file(const std::string & infile) {
        FILE * fp = fopen(infile.c_str(),"r");
        yaml_parser_t parser;
        yaml_event_t event;

        if (!fp) {
            std::cerr << "Cannot open yaml file '" << infile
                    << "': " << strerror(errno) << std::endl;
            return 1;
        }
        yaml_parser_initialize(&parser);
        yaml_parser_set_input_file(&parser, fp);
        state = START;
        do {
            if (!yaml_parser_parse(&parser, &event)) {
                state = STOP;
            }

            if (!consume_event(event)) {
                state = STOP;
            }

            if (accepted) {
                if(!consume_key_value(key, event)){
                    std::cerr << "Ignoring unknown key/value pair: " << key
                                << " = " << event.data.scalar.value << std::endl;
                }
            }
            yaml_event_delete(&event);
        } while (state != STOP);

        yaml_parser_delete(&parser);
        fclose(fp);
        return 0;
    }

protected:
    bool consume_key_value(const std::string & key, const yaml_event_t & event) {
        auto it = consumers.find(key);
        ConsumerClass * consumer = dynamic_cast<ConsumerClass*>(this);

        if(consumer) {
            if(it != consumers.end()) {
                //std::cerr << "Loading: " << key << std::endl;
                (consumer->*(it->second))(event);
                return true;
            }
            std::cerr << "UNKNOWN" << std::endl;
        } else {
            std::cerr << "ConsumerClass is not valid" << std::endl;
        }
        return false;
    }

    bool consume_event(yaml_event_t & event) {
        accepted = false;
        switch (state) {
        case START:
            switch (event.type) {
                case YAML_MAPPING_START_EVENT:
                    state = ACCEPT_KEY;
                    break;
                case YAML_SCALAR_EVENT:
                case YAML_SEQUENCE_START_EVENT:
                    state = ERROR;
                    break;
                case YAML_STREAM_END_EVENT:
                    state = STOP;
                    break;
                case YAML_STREAM_START_EVENT:
                case YAML_DOCUMENT_START_EVENT:
                case YAML_DOCUMENT_END_EVENT:
                    // ignore
                    break;
                default:
                    std::cerr << "UNHANDLED YAML EVENT:  " << event.type << std::endl;
                    state = ERROR;
                    break;
            }
            break;

        case ACCEPT_KEY:
            switch (event.type) {
                case YAML_SCALAR_EVENT:
                    key = (char *) event.data.scalar.value;
                    state = ACCEPT_VALUE;
                    break;
                case YAML_MAPPING_END_EVENT:
                    state = STOP;
                    break;
                default:
                    std::cerr << "UNHANDLED YAML EVENT (key): " << event.type
                            << "\nVALUE: " << event.data.scalar.value
                            << std::endl;
                    state = ERROR;
                    break;
            }
            break;

        case ACCEPT_VALUE:
            switch (event.type) {
                case YAML_SCALAR_EVENT:
                    accepted = true;
                    state = ACCEPT_KEY;
                    break;
                default:
                    std::cerr << "UNHANDLED YAML EVENT (value): " << event.type
                            << "\nVALUE: " << event.data.scalar.value
                            << std::endl;
                    state = ERROR;
                    break;
            }
            break;

        case ERROR:
        case STOP:
            break;
        }
        return (state != ERROR);
    }
};

class TestConfigReader : public YamlReader<TestConfigReader> {
    TestConfig & config;

public:
    TestConfigReader(TestConfig & config) : YamlReader(), config(config) {
        consumers["lammps_version"] = &TestConfigReader::lammps_version;
        consumers["date_generated"] = &TestConfigReader::date_generated;
        consumers["epsilon"]        = &TestConfigReader::epsilon;
        consumers["prerequisites"]  = &TestConfigReader::prerequisites;
        consumers["pre_commands"]   = &TestConfigReader::pre_commands;
        consumers["post_commands"]  = &TestConfigReader::post_commands;
        consumers["input_file"]     = &TestConfigReader::input_file;
        consumers["bond_style"]     = &TestConfigReader::bond_style;
        consumers["bond_coeff"]     = &TestConfigReader::bond_coeff;
        consumers["natoms"]         = &TestConfigReader::natoms;
        consumers["init_energy"]    = &TestConfigReader::init_energy;
        consumers["run_energy"]     = &TestConfigReader::run_energy;
        consumers["init_stress"]    = &TestConfigReader::init_stress;
        consumers["run_stress"]     = &TestConfigReader::run_stress;
        consumers["init_forces"]    = &TestConfigReader::init_forces;
        consumers["run_forces"]     = &TestConfigReader::run_forces;
    }

protected:

    void prerequisites(const yaml_event_t & event) {
        config.prerequisites.clear();
        std::stringstream data((char *)event.data.scalar.value);
        std::string line;

        while(std::getline(data, line, '\n')) {
            std::size_t found = line.find_first_of(" \t");
            std::string key = line.substr(0,found);
            found = line.find_first_not_of(" \t",found);
            // skip invalid data
            if (found == std::string::npos) {
                std::cerr << "Skipping invalid prerequisite line:\n"
                          << line << std::endl;
                continue;
            }
            std::string value = line.substr(found,line.find_first_of(" \t",found));
            test_config.prerequisites.push_back(std::pair<std::string,std::string>(key,value));
        }
    }
    void pre_commands(const yaml_event_t & event) {
        config.pre_commands.clear();
        std::stringstream data((char *)event.data.scalar.value);
        std::string line;

        while(std::getline(data, line, '\n')) {
            test_config.pre_commands.push_back(line);
        }
    }

    void post_commands(const yaml_event_t & event) {
        config.post_commands.clear();
        std::stringstream data((char *)event.data.scalar.value);
        std::string line;

        while (std::getline(data, line, '\n')) {
            test_config.post_commands.push_back(line);
        }
    }

    void lammps_version(const yaml_event_t & event) {
        config.lammps_version = (char *)event.data.scalar.value;
    }

    void date_generated(const yaml_event_t & event) {
        config.date_generated = (char *)event.data.scalar.value;
    }

    void epsilon(const yaml_event_t & event) {
        config.epsilon = atof((char *)event.data.scalar.value);
    }

    void input_file(const yaml_event_t & event) {
        config.input_file = (char *)event.data.scalar.value;
    }

    void bond_style(const yaml_event_t & event) {
        config.bond_style = (char *)event.data.scalar.value;
    }

    void bond_coeff(const yaml_event_t & event) {
        config.bond_coeff.clear();
        std::stringstream data((char *)event.data.scalar.value);
        std::string line;

        while (std::getline(data, line, '\n')) {
            test_config.bond_coeff.push_back(line);
        }
    }

    void natoms(const yaml_event_t & event) {
        config.natoms = atoi((char *)event.data.scalar.value);
    }

    void init_energy(const yaml_event_t & event) {
        config.init_energy = atof((char *)event.data.scalar.value);
    }

    void run_energy(const yaml_event_t & event) {
        config.run_energy = atof((char *)event.data.scalar.value);
    }

    void init_stress(const yaml_event_t & event) {
        stress_t stress;
        sscanf((char *)event.data.scalar.value,
                "%lg %lg %lg %lg %lg %lg",
                &stress.xx, &stress.yy, &stress.zz,
                &stress.xy, &stress.xz, &stress.yz);
        config.init_stress = stress;
    }

    void run_stress(const yaml_event_t & event) {
        stress_t stress;
        sscanf((char *)event.data.scalar.value,
                "%lg %lg %lg %lg %lg %lg",
                &stress.xx, &stress.yy, &stress.zz,
                &stress.xy, &stress.xz, &stress.yz);
        config.run_stress = stress;
    }

    void init_forces(const yaml_event_t & event) {
        config.init_forces.clear();
        config.init_forces.resize(config.natoms+1);
        std::stringstream data((const char*)event.data.scalar.value);
        std::string line;

        while(std::getline(data, line, '\n')) {
            int tag = 0;
            coord_t xyz;
            sscanf(line.c_str(), "%d %lg %lg %lg", &tag, &xyz.x, &xyz.y, &xyz.z);
            config.init_forces[tag] = xyz;
        }
    }

    void run_forces(const yaml_event_t & event) {
        config.run_forces.clear();
        config.run_forces.resize(config.natoms+1);
        std::stringstream data((char *)event.data.scalar.value);
        std::string line;

        while(std::getline(data, line, '\n')) {
            int tag;
            coord_t xyz;
            sscanf(line.c_str(), "%d %lg %lg %lg", &tag, &xyz.x, &xyz.y, &xyz.z);
            config.run_forces[tag] = xyz;
        }
    }
};

class YamlWriter {
    FILE *fp;
    yaml_emitter_t emitter;
    yaml_event_t   event;
public:
    YamlWriter(const char * outfile) {
        yaml_emitter_initialize(&emitter);
        fp = fopen(outfile, "w");
        if (!fp) {
            perror(__FILE__);
            return;
        }

        yaml_emitter_set_output_file(&emitter, fp);

        yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING);
        yaml_emitter_emit(&emitter, &event);
        yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 0);
        yaml_emitter_emit(&emitter, &event);
        yaml_mapping_start_event_initialize(&event, NULL,
                                            (yaml_char_t *)YAML_MAP_TAG,
                                            1, YAML_ANY_MAPPING_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }

    ~YamlWriter() {
        yaml_mapping_end_event_initialize(&event);
        yaml_emitter_emit(&emitter, &event);
        yaml_document_end_event_initialize(&event, 0);
        yaml_emitter_emit(&emitter, &event);
        yaml_stream_end_event_initialize(&event);
        yaml_emitter_emit(&emitter, &event);
        yaml_emitter_delete(&emitter);
        fclose(fp);
    }

    void emit(const std::string &key, const double value){
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *) key.c_str(),
                                    key.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
        char buf[256];
        snprintf(buf,256,"%.15g",value);
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *)buf,
                                    strlen(buf), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }

    void emit(const std::string &key, const long value){
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *) key.c_str(),
                                    key.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
        char buf[256];
        snprintf(buf,256,"%ld",value);
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *)buf,
                                    strlen(buf), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }

    void emit(const std::string &key, const int value){
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *) key.c_str(),
                                    key.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
        char buf[256];
        snprintf(buf,256,"%d",value);
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *)buf,
                                    strlen(buf), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }

    void emit(const std::string &key, const std::string &value)
    {
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *) key.c_str(),
                                    key.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *)value.c_str(),
                                    value.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }

    void emit_block(const std::string &key, const std::string &value){
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *) key.c_str(),
                                    key.size(), 1, 0,
                                    YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
        yaml_scalar_event_initialize(&event, NULL,
                                    (yaml_char_t *)YAML_STR_TAG,
                                    (yaml_char_t *)value.c_str(),
                                    value.size(), 1, 0,
                                    YAML_LITERAL_SCALAR_STYLE);
        yaml_emitter_emit(&emitter, &event);
    }
};

void generate(const char *outfile) {

    // initialize molecular system geometry
    const char *args[] = {"BondStyle", "-log", "none", "-echo", "screen", "-nocite" };
    char **argv = (char **)args;
    int argc = sizeof(args)/sizeof(char *);
    LAMMPS_NS::LAMMPS *lmp = init_lammps(argc,argv,test_config);
    if (!lmp) {
        std::cerr << "One ore more prerequisite styles are not available "
            "in this LAMMPS configuration:\n";
        for (auto prerequisite : test_config.prerequisites) {
            std::cerr << prerequisite.first << "_style "
                      << prerequisite.second << "\n";
        }
        return;
    }

    const int natoms = lmp->atom->natoms;
    const int bufsize = 256;
    char buf[bufsize];
    std::string block("");

    YamlWriter writer(outfile);

    // lammps_version
    writer.emit("lammps_version", lmp->universe->version);

    // date_generated
    std::time_t now = time(NULL);
    block = ctime(&now);
    block = block.substr(0,block.find("\n")-1);
    writer.emit("date_generated", block);

    // epsilon
    writer.emit("epsilon", test_config.epsilon);

    // prerequisites
    block.clear();
    for (auto prerequisite :  test_config.prerequisites) {
        block += prerequisite.first + " " + prerequisite.second + "\n";
    }
    writer.emit_block("prerequisites", block);

    // pre_commands
    block.clear();
    for (auto command :  test_config.pre_commands) {
        block += command + "\n";
    }
    writer.emit_block("pre_commands", block);

    // post_commands
    block.clear();
    for (auto command : test_config.post_commands) {
        block += command + "\n";
    }
    writer.emit_block("post_commands", block);

    // input_file
    writer.emit("input_file", test_config.input_file);

    // bond_style
    writer.emit("bond_style", test_config.bond_style);

    // bond_coeff
    block.clear();
    for (auto bond_coeff : test_config.bond_coeff) {
        block += bond_coeff + "\n";
    }
    writer.emit_block("bond_coeff", block);

    // natoms
    writer.emit("natoms", natoms);

    // init_energy
    writer.emit("init_energy", lmp->force->bond->energy);

    // init_stress
    double *stress = lmp->force->bond->virial;
    snprintf(buf,bufsize,"% 23.16e % 23.16e % 23.16e % 23.16e % 23.16e % 23.16e",
             stress[0],stress[1],stress[2],stress[3],stress[4],stress[5]);
    writer.emit_block("init_stress", buf);

    // init_forces
    block.clear();
    double **f = lmp->atom->f;
    LAMMPS_NS::tagint *tag = lmp->atom->tag;
    for (int i=0; i < natoms; ++i) {
        snprintf(buf,bufsize,"% 3d % 23.16e % 23.16e % 23.16e\n",
                 (int)tag[i], f[i][0], f[i][1], f[i][2]);
        block += buf;
    }
    writer.emit_block("init_forces", block);

    // do a few steps of MD
    run_lammps(lmp);

    // run_energy
    writer.emit("run_energy", lmp->force->bond->energy);

    // run_stress
    stress = lmp->force->bond->virial;
    snprintf(buf,bufsize,"% 23.16e % 23.16e % 23.16e % 23.16e % 23.16e % 23.16e",
             stress[0],stress[1],stress[2],stress[3],stress[4],stress[5]);
    writer.emit_block("run_stress", buf);

    block.clear();
    f = lmp->atom->f;
    tag = lmp->atom->tag;
    for (int i=0; i < natoms; ++i) {
        snprintf(buf,bufsize,"% 3d % 23.16e % 23.16e % 23.16e\n",
                 (int)tag[i], f[i][0], f[i][1], f[i][2]);
        block += buf;
    }
    writer.emit_block("run_forces", block);

    delete lmp;
    return;
}

TEST(BondStyle, plain) {
    const char *args[] = {"BondStyle", "-log", "none", "-echo", "screen", "-nocite" };
    char **argv = (char **)args;
    int argc = sizeof(args)/sizeof(char *);

    ::testing::internal::CaptureStdout();
    LAMMPS_NS::LAMMPS *lmp = init_lammps(argc,argv,test_config);
    std::string output = ::testing::internal::GetCapturedStdout();

    if (!lmp) {
        std::cerr << "One ore more prerequisite styles are not available "
            "in this LAMMPS configuration:\n";
        for (auto prerequisite : test_config.prerequisites) {
            std::cerr << prerequisite.first << "_style "
                      << prerequisite.second << "\n";
        }
        GTEST_SKIP();
    }

    EXPECT_THAT(output, StartsWith("LAMMPS ("));
    EXPECT_THAT(output, HasSubstr("Loop time"));

    // abort if running in parallel and not all atoms are local
    const int nlocal = lmp->atom->nlocal;
    ASSERT_EQ(lmp->atom->natoms,nlocal);

    double epsilon = test_config.epsilon;
    double **f=lmp->atom->f;
    LAMMPS_NS::tagint *tag=lmp->atom->tag;
    ErrorStats stats;
    stats.reset();
    const std::vector<coord_t> &f_ref = test_config.init_forces;
    ASSERT_EQ(nlocal+1,f_ref.size());
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_ref[tag[i]].x, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_ref[tag[i]].y, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_ref[tag[i]].z, epsilon);
    }
    if (print_stats)
        std::cerr << "init_forces stats:" << stats << std::endl;

    LAMMPS_NS::Bond *bond = lmp->force->bond;
    double *stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.init_stress.xx, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.init_stress.yy, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.init_stress.zz, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.init_stress.xy, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.init_stress.xz, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.init_stress.yz, epsilon);
    if (print_stats)
        std::cerr << "init_stress stats:" << stats << std::endl;

    stats.reset();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.init_energy, epsilon);
    if (print_stats)
        std::cerr << "init_energy stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    run_lammps(lmp);
    ::testing::internal::GetCapturedStdout();

    f = lmp->atom->f;
    stress = bond->virial;
    const std::vector<coord_t> &f_run = test_config.run_forces;
    ASSERT_EQ(nlocal+1,f_run.size());
    stats.reset();
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_run[tag[i]].x, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_run[tag[i]].y, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_run[tag[i]].z, 10*epsilon);
    }
    if (print_stats)
        std::cerr << "run_forces  stats:" << stats << std::endl;

    stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.run_stress.xx, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.run_stress.yy, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.run_stress.zz, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.run_stress.xy, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.run_stress.xz, epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.run_stress.yz, epsilon);
    if (print_stats)
        std::cerr << "run_stress  stats:" << stats << std::endl;

    stats.reset();
    int id = lmp->modify->find_compute("sum");
    double energy = lmp->modify->compute[id]->compute_scalar();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.run_energy, epsilon);
    EXPECT_FP_LE_WITH_EPS(bond->energy, energy, epsilon);
    if (print_stats)
        std::cerr << "run_energy  stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    delete lmp;
    ::testing::internal::GetCapturedStdout();
};

TEST(BondStyle, omp) {
    if (!LAMMPS_NS::LAMMPS::is_installed_pkg("USER-OMP")) GTEST_SKIP();
    const char *args[] = {"BondStyle", "-log", "none", "-echo", "screen",
                          "-nocite", "-pk", "omp", "4", "-sf", "omp"};
    char **argv = (char **)args;
    int argc = sizeof(args)/sizeof(char *);

    ::testing::internal::CaptureStdout();
    LAMMPS_NS::LAMMPS *lmp = init_lammps(argc,argv,test_config);
    std::string output = ::testing::internal::GetCapturedStdout();

    if (!lmp) {
        std::cerr << "One ore more prerequisite styles with /omp suffix\n"
            "are not available in this LAMMPS configuration:\n";
        for (auto prerequisite : test_config.prerequisites) {
            std::cerr << prerequisite.first << "_style "
                      << prerequisite.second << "\n";
        }
        GTEST_SKIP();
    }

    EXPECT_THAT(output, StartsWith("LAMMPS ("));
    EXPECT_THAT(output, HasSubstr("Loop time"));

    // abort if running in parallel and not all atoms are local
    const int nlocal = lmp->atom->nlocal;
    ASSERT_EQ(lmp->atom->natoms,nlocal);

    // relax error a bit for USER-OMP package
    double epsilon = 5.0*test_config.epsilon;
    double **f=lmp->atom->f;
    LAMMPS_NS::tagint *tag=lmp->atom->tag;
    const std::vector<coord_t> &f_ref = test_config.init_forces;
    ErrorStats stats;
    stats.reset();
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_ref[tag[i]].x, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_ref[tag[i]].y, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_ref[tag[i]].z, epsilon);
    }
    if (print_stats)
        std::cerr << "init_forces stats:" << stats << std::endl;

    LAMMPS_NS::Bond *bond = lmp->force->bond;
    double *stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.init_stress.xx, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.init_stress.yy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.init_stress.zz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.init_stress.xy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.init_stress.xz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.init_stress.yz, 10*epsilon);
    if (print_stats)
        std::cerr << "init_stress stats:" << stats << std::endl;

    stats.reset();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.init_energy, epsilon);
    if (print_stats)
        std::cerr << "init_energy stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    run_lammps(lmp);
    ::testing::internal::GetCapturedStdout();

    f = lmp->atom->f;
    stress = bond->virial;
    const std::vector<coord_t> &f_run = test_config.run_forces;
    ASSERT_EQ(nlocal+1,f_run.size());
    stats.reset();
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_run[tag[i]].x, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_run[tag[i]].y, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_run[tag[i]].z, 10*epsilon);
    }
    if (print_stats)
        std::cerr << "run_forces  stats:" << stats << std::endl;

    stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.run_stress.xx, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.run_stress.yy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.run_stress.zz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.run_stress.xy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.run_stress.xz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.run_stress.yz, 10*epsilon);
    if (print_stats)
        std::cerr << "run_stress  stats:" << stats << std::endl;

    stats.reset();
    int id = lmp->modify->find_compute("sum");
    double energy = lmp->modify->compute[id]->compute_scalar();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.run_energy, epsilon);
    EXPECT_FP_LE_WITH_EPS(bond->energy, energy, epsilon);
    if (print_stats)
        std::cerr << "run_energy  stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    delete lmp;
    ::testing::internal::GetCapturedStdout();
};

TEST(BondStyle, intel) {
    if (!LAMMPS_NS::LAMMPS::is_installed_pkg("USER-INTEL")) GTEST_SKIP();
    const char *args[] = {"BondStyle", "-log", "none", "-echo", "screen",
                          "-nocite", "-pk", "intel", "0", "mode", "double",
                          "omp", "4", "lrt", "no", "-sf", "intel"};
    char **argv = (char **)args;
    int argc = sizeof(args)/sizeof(char *);

    ::testing::internal::CaptureStdout();
    LAMMPS_NS::LAMMPS *lmp = init_lammps(argc,argv,test_config);
    std::string output = ::testing::internal::GetCapturedStdout();

    if (!lmp) {
        std::cerr << "One ore more prerequisite styles with /intel suffix\n"
            "are not available in this LAMMPS configuration:\n";
        for (auto prerequisite : test_config.prerequisites) {
            std::cerr << prerequisite.first << "_style "
                      << prerequisite.second << "\n";
        }
        GTEST_SKIP();
    }

    // relax error a bit for USER-INTEL package
    double epsilon = 5.0*test_config.epsilon;

    // we need to relax the epsilon a LOT for tests using long-range
    // coulomb with tabulation. seems more like mixed precision or a bug
    for (auto post_cmd : test_config.post_commands) {
        if (post_cmd.find("bond_modify table") != std::string::npos) {
            if (post_cmd.find("bond_modify table 0") == std::string::npos)
                epsilon *= 1000000.0;
        }
    }

    EXPECT_THAT(output, StartsWith("LAMMPS ("));
    EXPECT_THAT(output, HasSubstr("Loop time"));

    // abort if running in parallel and not all atoms are local
    const int nlocal = lmp->atom->nlocal;
    ASSERT_EQ(lmp->atom->natoms,nlocal);

    double **f=lmp->atom->f;
    LAMMPS_NS::tagint *tag=lmp->atom->tag;
    const std::vector<coord_t> &f_ref = test_config.init_forces;
    ErrorStats stats;
    stats.reset();
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_ref[tag[i]].x, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_ref[tag[i]].y, epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_ref[tag[i]].z, epsilon);
    }
    if (print_stats)
        std::cerr << "init_forces stats:" << stats << std::endl;

    LAMMPS_NS::Bond *bond = lmp->force->bond;
    double *stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.init_stress.xx, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.init_stress.yy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.init_stress.zz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.init_stress.xy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.init_stress.xz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.init_stress.yz, 10*epsilon);
    if (print_stats)
        std::cerr << "init_stress stats:" << stats << std::endl;

    stats.reset();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.init_energy, epsilon);
    if (print_stats)
        std::cerr << "init_energy stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    run_lammps(lmp);
    ::testing::internal::GetCapturedStdout();

    f = lmp->atom->f;
    stress = bond->virial;
    const std::vector<coord_t> &f_run = test_config.run_forces;
    ASSERT_EQ(nlocal+1,f_run.size());
    stats.reset();
    for (int i=0; i < nlocal; ++i) {
        EXPECT_FP_LE_WITH_EPS(f[i][0], f_run[tag[i]].x, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][1], f_run[tag[i]].y, 10*epsilon);
        EXPECT_FP_LE_WITH_EPS(f[i][2], f_run[tag[i]].z, 10*epsilon);
    }
    if (print_stats)
        std::cerr << "run_forces  stats:" << stats << std::endl;

    stress = bond->virial;
    stats.reset();
    EXPECT_FP_LE_WITH_EPS(stress[0], test_config.run_stress.xx, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[1], test_config.run_stress.yy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[2], test_config.run_stress.zz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[3], test_config.run_stress.xy, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[4], test_config.run_stress.xz, 10*epsilon);
    EXPECT_FP_LE_WITH_EPS(stress[5], test_config.run_stress.yz, 10*epsilon);
    if (print_stats)
        std::cerr << "run_stress  stats:" << stats << std::endl;

    stats.reset();
    int id = lmp->modify->find_compute("sum");
    double energy = lmp->modify->compute[id]->compute_scalar();
    EXPECT_FP_LE_WITH_EPS(bond->energy, test_config.run_energy, epsilon);
    EXPECT_FP_LE_WITH_EPS(bond->energy, energy, epsilon);
    if (print_stats)
        std::cerr << "run_energy  stats:" << stats << std::endl;

    ::testing::internal::CaptureStdout();
    delete lmp;
    ::testing::internal::GetCapturedStdout();
};

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);

    if ((argc != 2) && (argc != 4)) {
        std::cerr << "usage: " << argv[0] << " <testfile.yaml> "
            "[--gen <newfile.yaml> |"
            " --stats <yes|no>]" << std::endl;
        return 1;
    }

    auto reader = TestConfigReader(test_config);
    if (reader.parse_file(argv[1])) {
        std::cerr << "Error parsing yaml file: " << argv[1] << std::endl;
        return 2;
    }

    if (argc == 4) {
        if (strcmp(argv[2],"--gen") == 0) {
            generate(argv[3]);
            return 0;
        } else if (strcmp(argv[2],"--stats") == 0) {
            if (strcmp(argv[3],"yes") == 0) print_stats = true;
        } else {
            std::cerr << "usage: " << argv[0] << " <testfile.yaml> "
                "[--gen <newfile.yaml> |"
                " --stats <yes|no>]" << std::endl;
            return 1;
        }
    }
    return RUN_ALL_TESTS();
}
