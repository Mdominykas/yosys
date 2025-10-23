#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "libs/json11/json11.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

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


    void check_validity(RTLIL::Module *mod){
        assert(parsed);

        for(std::string ctr_input : control_inputs){
            Wire *wire = mod->wire(RTLIL::escape_id(ctr_input));
            assert(wire != nullptr);
            assert(wire->port_input);
        }

        for(std::string output_wire : {applicability, observation}){
            Wire *wire = mod->wire(RTLIL::escape_id(output_wire));
            assert(wire != nullptr);
            assert(wire->port_output);
        }

        assert((0 < expression_complexity) && (expression_complexity < 10));
    }
};


PRIVATE_NAMESPACE_END
