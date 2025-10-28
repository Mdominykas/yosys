#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "libs/json11/json11.hpp"

#include "passes/hw_sw_contracts/utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

using namespace hardware_software_contracts;


// TODO: make a common class for parsing json
struct SimplificationParameters{
    std::string simplified_module_name;

    std::string applicability, observation;
    vector<std::string> control_inputs;

    int expression_complexity, test_cnt;

    bool parsed;

    SimplificationParameters() {
        parsed = false;
    }


    void parse_parameters(std::string parameter_file){
        std::ifstream input(parameter_file);

        // check if file exists
        if(!input.good()){
            log_error("Invalid configuration file\n");
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string contents = buffer.str();

        std::string err = "";

        json11::Json json = Json::parse(contents, err);

        if(err != ""){
            log_error("Failure when reading a json file\n");
        }

        // parse predictor configuration
        if(!json.is_object()){
            log_error("Expected object in configuration file\n");
        }
        auto json_items = json.object_items();

        simplified_module_name = parse_string_from_object(json_items, "simplified_module_name");
        applicability = parse_string_from_object(json_items, "applicability");
        observation = parse_string_from_object(json_items, "observation");
        expression_complexity = parse_int_from_object(json_items, "expression_complexity");
        test_cnt = parse_int_from_object(json_items, "test_cnt");

        auto control_input_arr = parse_array_from_object(json_items, "control_inputs");
        for(auto ctr : control_input_arr){
            assert(ctr.is_string());
            control_inputs.push_back(ctr.string_value());
        }

        parsed = true;
    }

    Json::array parse_array_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_array()){
            const std::string error_msg = "Value of " + name + " must be of an array type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].array_items();
    }

    std::string cast_to_string(Json::object obj, std::string name){
        if(!obj[name].is_string()){
            const std::string error_msg = "Value of  " + name + " must be of a string type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].string_value();
    }

    std::string parse_string_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        return cast_to_string(obj, name);
    }

    int parse_int_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_number()){
            const std::string error_msg = "Value of  " + name + " must be of a int type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].int_value();
    }

    vector<Wire*> extract_control_wires(RTLIL::Module *predictor_mod){
        vector<Wire*> control_wires;

        for(std::string ctr_input : control_inputs){
            IdString input_wire_name = RTLIL::escape_id("inp_" + ctr_input);
            Wire *wire = predictor_mod->wire(input_wire_name);
            std::cout << "ieskau wire su pavadinimu: " << input_wire_name.str() << std::endl;
            if(wire == nullptr){
                IdString wire_name_in_level = RTLIL::escape_id(ctr_input + "_level_0");
                wire = predictor_mod->wire(wire_name_in_level);
                std::cout << "ieskau wire su levelio pavadinimu: " << input_wire_name.str() << std::endl;
                if(wire == nullptr){
                    std::cout << "No wire named something like: " << ctr_input << std::endl;
                }
                make_wire_input(predictor_mod, wire, RTLIL::escape_id(ctr_input));
                std::cout << "vel ieskau wire su pavadinimu: " << input_wire_name.str() << std::endl;
                wire = predictor_mod->wire(input_wire_name);
            }
            assert(wire != nullptr);
            assert(wire->port_input);
            control_wires.push_back(wire);
        }
        return control_wires;
    }

    void check_validity(RTLIL::Module *predictor_mod){
        assert(parsed);

        // for(Wire *wire : predictor_mod->wires()){
        //     std::cout << "Predictor has wire: " << wire->name.str() << std::endl;
        // }

        // std::cout << "Predictor has inputs: " << std::endl;
        // for(Wire *wire : predictor_mod->wires()){
        //     if(wire->port_input){
        //         std::cout << "wire: " << wire->name.str() << std::endl;
        //     }
        // }

        auto extracted = extract_control_wires(predictor_mod);
        // some random check to not have warning about unused element
        assert(extracted.size() == control_inputs.size());


        for(std::string output_wire : {applicability, observation}){
            Wire *wire = predictor_mod->wire(RTLIL::escape_id(output_wire));
            if(wire == nullptr){
                std::cout << "No wire named: " << output_wire << " found" << std::endl;
            }
            assert(wire != nullptr);
            assert(wire->port_output);
        }

        assert((0 < expression_complexity) && (expression_complexity < 10));
    }
};


PRIVATE_NAMESPACE_END
