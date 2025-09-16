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


struct PredictorConfiguration{
    std::string output_wire;
    vector<std::string> enter_wires, busy_wires, exit_wires;
    // some incorrect value for default constructor
    int prediction_bound = -1;
    int default_prediction = -1;
    PredictorConfiguration(std::string output_wire, vector<std::string> enter_wires, vector<std::string> busy_wires,
            vector<std::string> exit_wires, int prediction_bound, int default_prediction) : output_wire(output_wire), enter_wires(enter_wires), busy_wires(busy_wires), exit_wires(exit_wires),
            prediction_bound(prediction_bound), default_prediction(default_prediction){
        assert(prediction_bound >= 0);
        assert(!enter_wires.empty());
        assert(enter_wires.size() == busy_wires.size());
        assert(busy_wires.size() == exit_wires.size());
    }
    PredictorConfiguration() { }
};

struct ConfigurationFile {
    std::string clock_name;

    vector<PredictorConfiguration> predictors;

    ConfigurationFile() { }

    ConfigurationFile(std::string filename){
        std::ifstream input(filename);

        // check if file exists
        if(!input.good()){
            log_error("ERROR: invalid configuration file\n");
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string contents = buffer.str();

        std::string err = "";

        json11::Json json = Json::parse(contents, err);

        if(err != ""){
            log_error("ERROR: failure when reading a json file\n");
        }

        // parse predictor configuration
        if(!json.is_object()){
            log_error("ERROR: expected object in configuration file\n");
        }

        auto json_items = json.object_items();

        auto json_predictors = parse_array_from_object(json_items, "predictors");
        for(auto json_pred : json_predictors){
            predictors.push_back(parse_predictor_from_json(json_pred));
        }


        clock_name = parse_string_from_object(json_items, "clock_name");
    }

    Json::object parse_object_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "ERROR: value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_object()){
            const std::string error_msg = "ERROR: " + name + " must be of an object type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].object_items();
    }

    Json::array parse_array_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "ERROR: value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_array()){
            const std::string error_msg = "ERROR: " + name + " must be of an array type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].array_items();
    }

    std::string cast_to_string(Json::object obj, std::string name){
        if(!obj[name].is_string()){
            const std::string error_msg = "ERROR: " + name + " must be of a string type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].string_value();
    }

    std::string parse_string_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "ERROR: value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        return cast_to_string(obj, name);
    }

    int parse_int_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "ERROR: value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_number()){
            const std::string error_msg = "ERROR: " + name + " must be of a int type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].int_value();
    }

    PredictorConfiguration parse_predictor_from_json(Json predictor_json){
        if(!predictor_json.is_object()){
            log_error("ERROR: expected object in configuration file\n");
        }

        auto json_items = predictor_json.object_items();

        int prediction_bound = parse_int_from_object(json_items, "prediction_bound");
        int default_prediction = parse_int_from_object(json_items, "default_prediction");
        std::string output_wire = parse_string_from_object(json_items, "final_wire");

        vector<std::string> enter_wires, busy_wires, exit_wires;

        Json::array json_enter = parse_array_from_object(json_items, "enter_wires");
        for(Json val : json_enter){
            assert(val.is_string());
            enter_wires.push_back(val.string_value());
        }

        Json::array json_busy = parse_array_from_object(json_items, "busy_wires");
        for(Json val : json_busy){
            assert(val.is_string());
            busy_wires.push_back(val.string_value());
        }

        Json::array json_exit = parse_array_from_object(json_items, "exit_wires");
        for(Json val : json_exit){
            assert(val.is_string());
            exit_wires.push_back(val.string_value());
        }

        return PredictorConfiguration(output_wire, enter_wires, busy_wires, exit_wires, prediction_bound, default_prediction);
    }
};

PRIVATE_NAMESPACE_END
