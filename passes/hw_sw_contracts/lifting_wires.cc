#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "passes/hw_sw_contracts/configuration_file.cc"
#include "passes/hw_sw_contracts/my_json_parser.cc"

#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct SingleWireLifting{
    string inner_module;
    string outer_module;
    string instance_name;
    string inner_wire;
    string outer_wire;
    SingleWireLifting(string inner_module, string outer_module, string instance_name, string inner_wire, string outer_wire) : inner_module(inner_module), outer_module(outer_module), instance_name(instance_name), inner_wire(inner_wire), outer_wire(outer_wire) { }
};



struct WireLiftingConfiguration : public MyJsonParser{
    vector<SingleWireLifting> liftings;
    WireLiftingConfiguration() { }
    void add_lifting(SingleWireLifting lifting){
        liftings.push_back(lifting);
    }

    void parse_input_from_file(string filename){
        assert(liftings.empty());
        std::ifstream input(filename);

        if(!input.good()){
            log_error("Invalid configuration file\n");
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string contents = buffer.str();

        std::string err = "";

        json11::Json json = Json::parse(contents, err);

        if(err != ""){
            log_error("Failure when reading a json file\nThe error is: %s", err.c_str());
        }

        if(!json.is_object()){
            log_error("Expected object in configuration file\n");
        }

        auto json_items = json.object_items();

        auto lifting_list = parse_array_from_object(json_items, "liftings");

        for(auto lifting : lifting_list){
            string inner_module = parse_string_from_object(lifting.object_items(), "inner_module");
            string outer_module = parse_string_from_object(lifting.object_items(), "outer_module");
            string instance_name = parse_string_from_object(lifting.object_items(), "instance_name");
            string inner_wire = parse_string_from_object(lifting.object_items(), "inner_wire");
            string outer_wire = parse_string_from_object(lifting.object_items(), "outer_wire");
            add_lifting(SingleWireLifting(inner_module, outer_module, instance_name, inner_wire, outer_wire));
        }
    }
};

struct LiftingWires : public Pass {

    LiftingWires() : Pass("lifting_wires", "Takes json file as an argument and makes wires available in higher modules") { }

    void help() override
	{
		log("\n");
		log("    lifting_wires <configuration_file> \n");
		log("\n");
	}

    RTLIL::Module *find_unique_parameterized_module(RTLIL::Design *design, const std::string &module_name)
    {
        RTLIL::IdString wanted = RTLIL::escape_id(module_name);
        RTLIL::IdString exact = "";

        pool<RTLIL::IdString> candidates;

        for (RTLIL::Module *pos_mod : design->modules())
        {
            // Already non-parameterized.
            if (pos_mod->name == wanted) {
                exact = pos_mod->name;
                
                continue;
            }

            // Parameterized module.
            if(!pos_mod->name.begins_with("$paramod")){
                continue;
            }

            if (pos_mod->has_attribute(ID::hdlname))
            {
                std::string original_name = pos_mod->get_string_attribute(ID::hdlname);
                if (IdString(RTLIL::escape_id(original_name)) == wanted)
                    candidates.insert(pos_mod->name);
            }
        }

        if((candidates.empty()) && (exact != "")){
            candidates.insert(exact);
        }

        if (candidates.empty()) {
            log_error("Could not find a parameterized module corresponding to `%s'.\n",module_name.c_str());
        }

        if (candidates.size() != 1) {
            log("Found multiple elaborated variants of `%s':\n", module_name.c_str());

            for (auto type : candidates)
                log("    %s\n", type.c_str());

            log_error("Module `%s' does not have a unique elaborated variant.\n", module_name.c_str());
        }

        RTLIL::IdString result = *candidates.begin();

        RTLIL::Module *mod = design->module(result);
        log_assert(mod != nullptr);

        return mod;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
        log_header(design, "Executing LIFTING WIRES pass.\n");

        if(args.size() < 2){
			log_error("Incorrect number of arguments\n");
		}

        std::string configuration_file_name = args[1];

        WireLiftingConfiguration wire_lifting_configuration;
        wire_lifting_configuration.parse_input_from_file(configuration_file_name);

        std::set<Wire*> already_made_output;

        for(SingleWireLifting lifting : wire_lifting_configuration.liftings){
            RTLIL::Module *outer_mod = find_unique_parameterized_module(design, lifting.outer_module);
            RTLIL::Module *inner_mod = find_unique_parameterized_module(design, lifting.inner_module);

            RTLIL::Wire *inner_wire = inner_mod->wire(RTLIL::escape_id(lifting.inner_wire));
            if(inner_wire == nullptr){
                for(auto wire : inner_mod->wires()){
                    std::cout << "have wire:" << wire->name.str() << std::endl;
                }
                log_error("Wire named: %s doesn't exist", lifting.inner_wire.c_str());
            }

            if (inner_wire->port_input && !inner_wire->port_output) {
                log_error("Trying to lift %s.%s, but it is already an INPUT port.\n", lifting.inner_module.c_str(), lifting.inner_wire.c_str());
            }



            if((already_made_output.find(inner_wire) == already_made_output.end()) && (inner_wire->port_output)){
                log_error("Trying to make wire %s into output, but it already was an output. So setting a new one will disconnect something", inner_wire->name.c_str());
            }
            inner_wire->port_output = true;
            inner_mod->fixup_ports();
            already_made_output.insert(inner_wire);

            RTLIL::Wire *outer_wire = outer_mod->wire(RTLIL::escape_id(lifting.outer_wire));
            if(outer_wire == nullptr){
                outer_wire = outer_mod->addWire(RTLIL::escape_id(lifting.outer_wire), inner_wire->width);
            }
            else{
                if (outer_wire != nullptr && outer_wire->width != inner_wire->width) {
                    log_error("Width mismatch: %s.%s is %d bits, but %s.%s is %d bits.\n",
                    lifting.inner_module.c_str(), lifting.inner_wire.c_str(), inner_wire->width,
                    lifting.outer_module.c_str(), lifting.outer_wire.c_str(), outer_wire->width);
                }
            }

            RTLIL::Cell *inner_cell = outer_mod->cell(RTLIL::escape_id(lifting.instance_name));
            assert(inner_cell != nullptr);

            // !!!!!! SITO NEDARYTI NEBENT IS ANKSCIAU PASETINTAS ARBA NERA OUTPUTAS
            inner_cell->setPort(RTLIL::escape_id(lifting.inner_wire), RTLIL::SigSpec(outer_wire));

            RTLIL::IdString port_name = RTLIL::escape_id(lifting.inner_wire);

            std::cout << "I will do something with wires named: " << inner_wire->name.str() << " and " << outer_wire->name.str() << std::endl;
        }
    }

} LiftingWires;


PRIVATE_NAMESPACE_END
